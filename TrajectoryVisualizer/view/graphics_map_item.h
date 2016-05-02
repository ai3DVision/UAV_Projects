#ifndef GRAPHICS_MAP_ITEM_H
#define GRAPHICS_MAP_ITEM_H

#include <QGraphicsItemGroup>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>

#include "graphics_orientation_item.h"

namespace viewpkg
{

    class GraphicsMapItem : public QGraphicsItemGroup
    {
    public:
        GraphicsMapItem();
        GraphicsMapItem(const QPixmap &map);

        void setMapItem(const QPixmap &pixmap);

        void setHoverable(bool is_hoverable);

        QGraphicsPixmapItem& getMapItem() { return map_item; }
        const QGraphicsPixmapItem& getMapItem() const { return map_item; }

    protected:
        void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
        void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;

    private:
        QGraphicsPixmapItem         map_item;
        QGraphicsRectItem           bound_item;
        //GraphicsOrientationItem     orient_item;

        //bool is_hoverable;
    };

}

#endif // GRAPHICS_MAP_ITEM_H
