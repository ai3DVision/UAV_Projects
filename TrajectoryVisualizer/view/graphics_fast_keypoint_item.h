#ifndef GRAPHICSFASTKEYPOINTITEM_H
#define GRAPHICSFASTKEYPOINTITEM_H

#include <QGraphicsItem>

namespace viewpkg
{

class GraphicsFastKeyPointItem : public QGraphicsItem
{
 public:
  GraphicsFastKeyPointItem(QPointF center = QPointF(0, 0), double angle = 0,
                           double radius = 1, QGraphicsItem *parent = 0);

  void setCenter(QPointF center);
  void setRadius(double radius);
  void setColor(QColor color);
  void setAngle(double angle);

 public:
  QRectF boundingRect() const override;
  void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
             QWidget *widget) override;

 private:
  QPointF center;
  double angle;
  double radius;

  QColor color;
};

}

#endif // GRAPHICSFASTKEYPOINTITEM_H
