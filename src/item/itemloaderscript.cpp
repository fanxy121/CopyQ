/*
    Copyright (c) 2017, Lukas Holecek <hluk@email.cz>

    This file is part of CopyQ.

    CopyQ is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    CopyQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CopyQ.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "itemloaderscript.h"

#include "common/commandstatus.h"
#include "common/contenttype.h"
#include "common/log.h"
#include "gui/icons.h"
#include "scriptable/scriptable.h"
#include "scriptable/scriptableproxy.h"

#include <QFileInfo>
#include <QRegExp>
#include <QScriptEngine>

namespace {

const char scriptFunctionName[] = "copyq_script";

class ItemScriptableScript : public ItemScriptable
{
    Q_OBJECT
public:
    ItemScriptableScript(const QString &script, QObject *parent)
        : ItemScriptable(parent)
        , m_script(script)
    {
    }

    void start() override
    {
        eval(m_script);
    }

private:
    QString m_script;
};

class ItemSaverScript : public ItemSaverInterface
{
public:
    ItemSaverScript(const ItemSaverPtr &saver, const QScriptValue &obj, Scriptable *scriptable)
        : m_saver(saver)
        , m_obj(obj)
        , m_scriptable(scriptable)
    {
    }

    bool saveItems(const QString &tabName, const QAbstractItemModel &model, QIODevice *file) override
    {
        return m_saver->saveItems(tabName, model, file);
    }

    bool canRemoveItems(const QList<QModelIndex> &indexList, QString *error) override
    {
        return m_saver->canRemoveItems(indexList, error);
    }

    bool canMoveItems(const QList<QModelIndex> &indexList) override
    {
        return m_saver->canMoveItems(indexList);
    }

    void itemsRemovedByUser(const QList<QModelIndex> &indexList) override
    {
        m_saver->itemsRemovedByUser(indexList);
    }

    QVariantMap copyItem(const QAbstractItemModel &model, const QVariantMap &itemData) override
    {
        auto itemData2 = m_saver->copyItem(model, itemData);

        transformItemData("copyItem", &itemData2);
        return itemData2;
    }

    void transformItemData(const QAbstractItemModel &model, QVariantMap *itemData) override
    {
        m_saver->transformItemData(model, itemData);
        transformItemData("transformItemData", itemData);
    }

private:
    void transformItemData(const QString &fnName, QVariantMap *itemData)
    {
        auto fn = m_obj.property(fnName);
        if ( !fn.isFunction() )
            return;

        const auto args = QScriptValueList() << m_scriptable->fromDataMap(*itemData);
        const auto result = fn.call(m_obj, args);
        if ( result.isUndefined() || result.isNull() )
            return;

        *itemData = m_scriptable->toDataMap(result);
    }

    ItemSaverPtr m_saver;
    QScriptValue m_obj;
    Scriptable *m_scriptable;
};

class ItemLoaderScript : public QObject, public ItemLoaderInterface
{
    Q_OBJECT
    Q_INTERFACES(ItemLoaderInterface)

public:
    ItemLoaderScript(const QString &scriptFilePath, ScriptableProxy *proxy)
        : m_engine()
        , m_scriptable(&m_engine, proxy)
        , m_baseName( QFileInfo(scriptFilePath).baseName() )
        , m_id( m_baseName.replace(QRegExp("[^a-zA-Z0-9_]"), "_") )
    {
        QObject::connect( &m_scriptable, SIGNAL(sendMessage(QByteArray,int)),
                          this, SLOT(sendMessage(QByteArray,int)) );

        {
            QFile scriptFile(scriptFilePath);
            if ( !scriptFile.open(QIODevice::ReadOnly) ) {
                log( QString("Failed to open \"%1\": %2")
                     .arg(scriptFilePath, scriptFile.errorString()),
                     LogError );
            }

            m_script = scriptFile.readAll();
        }

        if ( !m_script.isEmpty() ) {
            m_scriptable.eval(m_script, scriptFilePath);
            m_obj = m_engine.globalObject().property(scriptFunctionName);
            if (m_obj.isFunction())
                m_obj = m_obj.call();

            if ( processUncaughtException() )
                m_obj = QScriptValue();
        }
    }

    /**
     * Returns true only if script was successfully loaded.
     */
    bool isLoaded() const
    {
        return m_obj.isObject();
    }

    int priority() const override { return 20; }

    QString id() const override
    {
        return m_id;
    }

    QString name() const override
    {
        return stringValue("name", m_baseName);
    }

    QString author() const override
    {
        return stringValue("author");
    }

    QString description() const override
    {
        return stringValue("description");
    }

    QVariant icon() const override
    {
        return IconCog;
    }

    QStringList formatsToSave() const override
    {
        return value("formatsToSave").toVariant().toStringList();
    }

    ItemSaverPtr transformSaver(const ItemSaverPtr &saver, QAbstractItemModel *) override
    {
        if ( m_obj.property("copyItem").isFunction()
             || m_obj.property("transformItemData").isFunction() )
        {
            return std::make_shared<ItemSaverScript>(saver, m_obj, &m_scriptable);
        }

        return saver;
    }

    ItemScriptable *scriptableObject(QObject *parent) override
    {
        return new ItemScriptableScript(m_script, parent);
    }

private slots:
    void sendMessage(const QByteArray &message, int messageCode)
    {
        if ( !message.isEmpty() ) {
            const auto text = QString::fromUtf8(message);
            switch (messageCode) {
            case CommandError:
            case CommandBadSyntax:
            case CommandException:
                log(text, LogWarning);
                break;
            default:
                log(text);
                break;
            }
        }
    }

private:
    QString stringValue(const QString &variableName, const QString &defaultValue = QString()) const
    {
        const auto value = this->value(variableName);

        if ( value.isValid() ) {
            const auto text = value.toString();
            if ( !text.isNull() )
                return text;
        }

        return defaultValue;
    }

    QScriptValue value(const QString &variableName) const
    {
        auto value = m_obj.property(variableName);
        if ( value.isFunction() )
            value = value.call(m_obj, QScriptValueList());

        if ( processUncaughtException() )
            return QScriptValue();

        return value;
    }

    bool processUncaughtException() const
    {
        if ( !m_engine.hasUncaughtException() )
            return false;

        log( m_engine.uncaughtException().toString(), LogWarning );
        m_scriptable.engine()->clearExceptions();
        return true;
    }

    void log(QString text, LogLevel level = LogNote) const
    {
        const auto label = "scripts::" + m_id + ": ";
        text.prepend(label);
        text.replace("\n", label);
        ::log(text, level);
    }

    QScriptEngine m_engine;
    Scriptable m_scriptable;
    QScriptValue m_obj;
    QString m_baseName;
    QString m_script;
    QString m_id;
};

} // namespace

ItemLoaderPtr createItemLoaderScript(const QString &scriptFilePath, ScriptableProxy *proxy)
{
    auto loader = std::make_shared<ItemLoaderScript>(scriptFilePath, proxy);
    if ( loader->isLoaded() )
        return loader;
    return nullptr;
}

#include "itemloaderscript.moc"
