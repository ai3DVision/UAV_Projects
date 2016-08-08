#include "graphics_map_view.h"

#include <QMatrix>
#include <QDebug>

#include "main_view.h"

using namespace viewpkg;

GraphicsMapView::GraphicsMapView(QWidget *parent)
  : QGraphicsView(parent), main_view(nullptr), zoom(1)
{
}

void GraphicsMapView::wheelEvent(QWheelEvent *event)
{
  if (event->modifiers() & Qt::ControlModifier)
  {
    //qDebug() << "wheel event + ctrl";
    if (event->delta() > 0)
    {
      //zoom++
      addZoom(zoom*0.2);
    }
    else
    {
      //zoom--
      addZoom(-zoom*0.2);
    }
    event->accept();
  }
  else
  {
    QGraphicsView::wheelEvent(event);
  }
}

void GraphicsMapView::addZoom(double value)
{
  setZoom(zoom + value);
}

void GraphicsMapView::setZoom(double zoom)
{
  this->zoom = zoom;
  updateMatrix();

  if (main_view)
  {
    main_view->setZoom(zoom);
  }
}

void GraphicsMapView::updateMatrix()
{
  QMatrix matrix;

  matrix.scale(zoom, zoom);

  setMatrix(matrix);
}
