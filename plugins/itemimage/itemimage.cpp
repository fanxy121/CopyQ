/*
    Copyright (c) 2014, Lukas Holecek <hluk@email.cz>

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

#include "itemimage.h"
#include "ui_itemimagesettings.h"

#include "common/contenttype.h"
#include "common/mimetypes.h"
#include "item/itemeditor.h"

#include <QBuffer>
#include <QHBoxLayout>
#include <QModelIndex>
#include <QMovie>
#include <QPainter>
#include <QPixmap>
#include <QtPlugin>
#include <QVariant>

namespace {

QString findImageFormat(const QList<QString> &formats)
{
    // Check formats in this order.
    static const QStringList imageFormats = QStringList()
            << QString("image/png")
            << QString("image/bmp")
            << QString("image/jpeg")
            << QString("image/gif")
            << QString("image/svg+xml");

    for (const auto &format : imageFormats) {
        if ( formats.contains(format) )
            return format;
    }

    return QString();
}

bool getImageData(const QVariantMap &dataMap, QByteArray *data, QString *mime)
{
    *mime = findImageFormat(dataMap.keys());
    if ( mime->isEmpty() )
        return false;

    *data = dataMap[*mime].toByteArray();

    return true;
}

bool getAnimatedImageData(const QVariantMap &dataMap, QByteArray *data, QByteArray *format)
{
    for (const auto &movieFormat : QMovie::supportedFormats()) {
        const QByteArray mime = "image/" + movieFormat;
        if (dataMap.contains(mime)) {
            *format = movieFormat;
            *data = dataMap[mime].toByteArray();
            return true;
        }
    }

    return false;
}

bool getPixmapFromData(const QVariantMap &dataMap, QPixmap *pix)
{
    QString mime;
    QByteArray data;
    if ( !getImageData(dataMap, &data, &mime) )
        return false;

    pix->loadFromData( data, mime.toLatin1() );

    return true;
}

} // namespace

ItemImage::ItemImage(
        const QPixmap &pix,
        const QByteArray &animationData, const QByteArray &animationFormat,
        const QString &imageEditor, const QString &svgEditor,
        QWidget *parent)
    : QLabel(parent)
    , ItemWidget(this)
    , m_editor(imageEditor)
    , m_svgEditor(svgEditor)
    , m_pixmap(pix)
    , m_animationData(animationData)
    , m_animationFormat(animationFormat)
    , m_animation(nullptr)
{
    setMargin(4);
    setPixmap(pix);
}

QObject *ItemImage::createExternalEditor(const QModelIndex &index, QWidget *parent) const
{
    QString mime;
    QByteArray data;
    const auto dataMap = index.data(contentType::data).toMap();
    if ( !getImageData(dataMap, &data, &mime) )
        return nullptr;

    const QString &cmd = mime.contains("svg") ? m_svgEditor : m_editor;

    return cmd.isEmpty() ? nullptr : new ItemEditor(data, mime, cmd, parent);
}

void ItemImage::updateSize(QSize, int)
{
    const auto m2 = 2 * margin();
#if QT_VERSION < 0x050000
    setFixedSize( m_pixmap.size() + QSize(m2, m2) );
#else
    const int ratio = devicePixelRatio();
    const int w = (m_pixmap.width() + 1) / ratio + m2;
    const int h = (m_pixmap.height() + 1) / ratio + m2;
    setFixedSize( QSize(w, h) );
#endif
}

void ItemImage::setCurrent(bool current)
{
    if (current) {
        if ( !m_animationData.isEmpty() ) {
            if (!m_animation) {
                QBuffer *stream = new QBuffer(&m_animationData, this);
                m_animation = new QMovie(stream, m_animationFormat, this);
                m_animation->setScaledSize( m_pixmap.size() );
            }

            if (m_animation) {
                setMovie(m_animation);
                startAnimation();
                m_animation->start();
            }
        }
    } else {
        stopAnimation();
        setPixmap(m_pixmap);
    }
}

void ItemImage::showEvent(QShowEvent *event)
{
    startAnimation();
    QLabel::showEvent(event);
}

void ItemImage::hideEvent(QHideEvent *event)
{
    QLabel::hideEvent(event);
    stopAnimation();
}

void ItemImage::paintEvent(QPaintEvent *event)
{
#if QT_VERSION < 0x050000
    QLabel::paintEvent(event);
#else
    // WORKAROUND: Draw current animation frame with correct DPI.
    if (movie()) {
        QPainter painter(this);
        auto pix = m_animation->currentPixmap();
        pix.setDevicePixelRatio( devicePixelRatio() );
        const auto m = margin();
        painter.drawPixmap(m, m, pix);
    } else {
        QLabel::paintEvent(event);
    }
#endif
}

void ItemImage::startAnimation()
{
    if (movie())
        movie()->start();
}

void ItemImage::stopAnimation()
{
    if (movie())
        movie()->stop();
}

ItemImageLoader::ItemImageLoader()
{
}

ItemImageLoader::~ItemImageLoader() = default;

ItemWidget *ItemImageLoader::create(const QVariantMap &data, QWidget *parent, bool preview) const
{
    if ( data.value(mimeHidden).toBool() )
        return nullptr;

    // TODO: Just check if image provided and load it in different thread.
    QPixmap pix;
    if ( !getPixmapFromData(data, &pix) )
        return nullptr;

#if QT_VERSION >= 0x050000
    pix.setDevicePixelRatio( parent->devicePixelRatio() );
#endif

    // scale pixmap
    const int w = preview ? 0 : m_settings.value("max_image_width", 320).toInt();
    const int h = preview ? 0 : m_settings.value("max_image_height", 240).toInt();
    if ( w > 0 && pix.width() > w && (h <= 0 || 1.0 * pix.width()/w > 1.0 * pix.height()/h) ) {
        pix = pix.scaledToWidth(w, Qt::SmoothTransformation);
    } else if (h > 0 && pix.height() > h) {
        pix = pix.scaledToHeight(h, Qt::SmoothTransformation);
    }

    QByteArray animationData;
    QByteArray animationFormat;
    getAnimatedImageData(data, &animationData, &animationFormat);

    return new ItemImage(pix,
                         animationData, animationFormat,
                         m_settings.value("image_editor").toString(),
                         m_settings.value("svg_editor").toString(), parent);
}

QStringList ItemImageLoader::formatsToSave() const
{
    return QStringList()
            << QString("image/svg+xml")
            << QString("image/png")
            << QString("image/gif");
}

QVariantMap ItemImageLoader::applySettings()
{
    m_settings["max_image_width"] = ui->spinBoxImageWidth->value();
    m_settings["max_image_height"] = ui->spinBoxImageHeight->value();
    m_settings["image_editor"] = ui->lineEditImageEditor->text();
    m_settings["svg_editor"] = ui->lineEditSvgEditor->text();
    return m_settings;
}

QWidget *ItemImageLoader::createSettingsWidget(QWidget *parent)
{
    ui.reset(new Ui::ItemImageSettings);
    QWidget *w = new QWidget(parent);
    ui->setupUi(w);
    ui->spinBoxImageWidth->setValue( m_settings.value("max_image_width", 320).toInt() );
    ui->spinBoxImageHeight->setValue( m_settings.value("max_image_height", 240).toInt() );
    ui->lineEditImageEditor->setText( m_settings.value("image_editor", "").toString() );
    ui->lineEditSvgEditor->setText( m_settings.value("svg_editor", "").toString() );
    return w;
}

Q_EXPORT_PLUGIN2(itemimage, ItemImageLoader)
