#ifndef IJABSTRACTCARTESIANGRID_H
#define IJABSTRACTCARTESIANGRID_H

#include "ijspatiallocation.h"

/**
 * The IJAbstractCartesianGrid class represents a Cartesian grid in the Image Jockey sub-system.
 */
class IJAbstractCartesianGrid
{
public:
    IJAbstractCartesianGrid();
	virtual ~IJAbstractCartesianGrid(){}

    /** Returns the length of the diagonal of the grid's box. */
    virtual double getDiagonalLength() = 0;

    /** Returns the rotation about the Z-axis in the grid's origin. */
    virtual double getRotation() = 0;

    //@{
    /** Getters for the spatial location of the center of the grid's box. */
    virtual double getCenterX() = 0;
    virtual double getCenterY() = 0;
    virtual double getCenterZ() = 0;
	virtual IJSpatialLocation getCenterLocation() = 0;
    //@}

    //@{
    /** Getters for the grid parameters. */
    virtual int getNI() = 0;
    virtual int getNJ() = 0;
    virtual int getNK() = 0;
    virtual double getCellSizeI() = 0;
    virtual double getCellSizeJ() = 0;
    virtual double getCellSizeK() = 0;
    virtual double getOriginX() = 0;
    virtual double getOriginY() = 0;
    virtual double getOriginZ() = 0;
    //@}

    /** Returns the value of the variable given its index and the topological coordinate. */
    virtual double getData( int variableIndex, int i, int j, int k ) = 0;

    /** Returns whether the given value is considered uninformed datum. */
    virtual bool isNoDataValue( double value ) = 0;

    /** Returns the value of the variable (given by its zero-based index) at the given spatial location.
     *  The function returns the value of grid cell that contains the given location.  The z coordinate is ignored
     * if the grid is 2D.
     * Make sure you load/select the desired realization prior to calling this in multi-realization grids.
     */
    virtual double getDataAt( int dataColumn, double x, double y, double z ) = 0;

    /**
     * Returns the maximum absolute value in the given column.
     * First column is 0.
     */
    virtual double absMax( int column ) = 0;

    /**
     * Returns the minimum absolute value in the given column.
     * First column is 0.
     */
    virtual double absMin( int column ) = 0;
};

#endif // IJABSTRACTCARTESIANGRID_H
