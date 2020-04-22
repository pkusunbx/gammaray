#ifndef VERTICALPROPORTIONCURVESPLOT_H
#define VERTICALPROPORTIONCURVESPLOT_H

#include <qwt_plot.h>

class QwtPlotCurve;
class QwtPlotIntervalCurve;

class VerticalProportionCurvesPlot : public QwtPlot
{

    Q_OBJECT

public:

    VerticalProportionCurvesPlot( QWidget *parent = nullptr );

    /**
     * Sets the number of curves used to divide the plot canvas.
     * For example, if you have three facies, you need two curves to divide the
     * plot area into three proportion ranges.
     */
    void setNumberOfCurves( size_t number );

    /**
     * Fills the plot area with the given color between the calibration curve given by base_curve index and the one
     * immediately to the right of it.  If base_curve is -1 or less, the "base curve" will be the plot left edge.
     * If base_curve is the index of the highest one or higher, the "rightmost curve" will be the plot right edge.
     * @param label Text that will appear in the plot legend (e.g. Sandstone).
     */
    void fillColor(const QColor &color, int base_curve , const QString label );

    /** Sets the given curve base, resulting in a flat line at the given value.
      *  The value must be between 0.0 and 100.0 (will be truncated otherwise).
      * Other curves will also be adjusted to prevent crossing.
      */
    void setCurveBase( int index, double value );

    /** Changes the geometry of the fill areas to match the calibration curves.
     * Does nothing if there are no fill areas between the curves.
     */
    void updateFillAreas();

public slots:
    void insertCurve( int axis, double base );

private:

    void insertCurve(Qt::Orientation o, const QColor &c, double base , QString label);

    /** Removes all curves currently in the plot. */
    void clearCurves();

    /** Removes the filled areas between the calibration curves. */
    void clearFillAreas();

    /**
     *  Updates the curves other than the given one so they do not cross each other.
     */
    void pushCurves( QwtPlotCurve* curve );

    /** The number of curves desired by the user. */
    size_t m_nCurves;

    /** The number of points in the curves. */
    size_t m_nPoints;

    /** The curves used to set proportion values. */
    std::vector<QwtPlotCurve*> m_curves;

    /** The filled areas between the calibration curves, normally used to calibrate
     * soft indicators for categorical variables. */
    std::vector<QwtPlotIntervalCurve*> m_fillAreas;

    /** The size of the curve handles in pixels. */
    size_t m_handleSize;

    /** The size of the legend icons in pixels. */
    size_t m_legendIconSize;
};

#endif // VERTICALPROPORTIONCURVESPLOT_H
