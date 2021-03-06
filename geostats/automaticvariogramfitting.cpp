#include "automaticvariogramfitting.h"

#include "domain/attribute.h"
#include "domain/file.h"
#include "domain/cartesiangrid.h"
#include "domain/application.h"
#include "dialogs/emptydialog.h"
#include "imagejockey/widgets/ijgridviewerwidget.h"
#include "imagejockey/svd/svdfactor.h"
#include "imagejockey/imagejockeyutils.h"
#include "imagejockey/svd/svdfactortree.h"
#include "imagejockey/svd/svdanalysisdialog.h"
#include "mainwindow.h"
#include "util.h"

#include <QProgressDialog>
#include <QMessageBox>
#include <cassert>
#include <thread>
#include <mutex>
#include <functional>
#include <QInputDialog>
#include <QCoreApplication>
#include <QApplication>
#include <QMessageBox>
#include <QChart>
#include <QLineSeries>
#include <QChartView>
#include <QValueAxis>

std::vector< double > AutomaticVariogramFitting::s_objectiveFunctionValues;

/** This is a mutex to restrict access to the FFTW routines from
 * multiple threads.  Some of its routines are not thread safe.*/
std::mutex myMutexFFTW; //ATTENTION NAME CLASH: there is a variable called mutexFFTW defined somewhere out there.
std::mutex myMutexLSRS;
std::mutex myMutexObjectiveFunction;

/**
 * The code for multithreaded gradient vector calculation for objective function.
 * @param vw Linear array with all the variographic parameters: [axis0,ratio0,az0,cc0,axis1,ratio1,...]
 * @param parameterIndexBin Vector with the indexes of  the variographic parameters this task can work with.
 * @param epsilon The "small value" used to compute a derivative numerically.
 * @param gridWithGeometry The grid object with the target grid geometry.
 * @param gridData The grid data (data count must be consistent with the grid geometry of gridWithGeometry).
 * @param m The number of nested variogram structures.
 * @param autoVarFitRef A reference to the automaitic variogram fitting object to call its objective function.
 * @param gradient a linear array containing the partial derivatives with respect to each parameter.
 */
void taskOnePartialDerivative (
                               const spectral::array& vw,
                               const std::vector< int >& parameterIndexBin,
                               const double epsilon,
                               IJAbstractCartesianGrid* gridWithGeometry,
                               const spectral::array& gridData,
                               const int m,
                               const AutomaticVariogramFitting* autoVarFitRef,
                               spectral::array* gradient //output object: for some reason, the thread object constructor does not compile with non-const references.
                               ){
    std::vector< int >::const_iterator it = parameterIndexBin.cbegin();
    for(; it != parameterIndexBin.cend(); ++it ){
        int iParameter = *it;
        //Make a set of parameters slightly shifted to the right (more positive) along one parameter.
        spectral::array vwFromRight( vw );
        vwFromRight(iParameter) = vwFromRight(iParameter) + epsilon;
        //Make a set of parameters slightly shifted to the left (more negative) along one parameter.
        spectral::array vwFromLeft( vw );
        vwFromLeft(iParameter) = vwFromLeft(iParameter) - epsilon;
        //Compute (numerically) the partial derivative with respect to one parameter.
        (*gradient)(iParameter) = ( autoVarFitRef->objectiveFunction( *gridWithGeometry, gridData, vwFromRight, m )
                                     -
                                    autoVarFitRef->objectiveFunction( *gridWithGeometry, gridData, vwFromLeft , m ))
                                     /
                                   ( 2 * epsilon );
    }
}

/**
 * The code for multithreaded moving of points along lines for the LSRS algorithm.
 * @param autoVarFitRef Reference to the AutomaticVarariogramFitting object so its objective function can be called.
 * @param m Number of variogram nested structures.
 * @param initial_i First point index to process.
 * @param final_i Last point index to process.  If final_i==initial_i, this thread processes only one point.
 * @param k Optimization step number. First must be 1, not 0.
 * @param domain The min/max variogram parameters boundaries as an object.
 * @param L_wMax The min variogram parameters boundaries as a linear array.
 * @param L_wMin The max variogram parameters boundaries as a linear array.
 * @param inputGrid The grid object with grid geometry.
 * @param inputData The grid input data.
 * @param randSequence Sequence of values returned by std::rand()/(double)RAND_MAX calls made before hand.  Its number of elements must be
 *                     Number of optimization steps * startingPoints.size() * vw_bestSolution.size()
 *                     A prior random number generation is to preserve the same random walk for a given seed
 *                     independently of number and order of multiple threads execution.
 * OUTPUT PARAMETERS:
 * @param startingPoints The set of points (solutions) that will travel along lines.
 * @param fOfBestSolution The value of objetive function at the best solution found.
 * @param vw_bestSolution The variogram parameters of the best solution found (as linear array).
 */
void taskMovePointAlongLineForLSRS (
        const AutomaticVariogramFitting& autoVarFitRef,
        int m,
        int initial_i,
        int final_i,
        int k,
        const VariogramParametersDomain &domain,
        const spectral::array& L_wMax,
        const spectral::array& L_wMin,
        const IJAbstractCartesianGrid& inputGrid,
        const spectral::array& inputData,
        const spectral::array& randSequence,
        std::vector< spectral::array >& startingPoints,
        double& fOfBestSolution,
        spectral::array& vw_bestSolution
        ){
    for( int i = initial_i; i <= final_i ; ++i )
        autoVarFitRef.movePointAlongLineForLSRS
                                 ( m, i, k, domain, L_wMax, L_wMin, inputGrid, inputData, randSequence, //<-- INPUT PARAMETERS
                                   startingPoints, fOfBestSolution, vw_bestSolution );                  //--> OUTPUT PARAMETERS
}

////////////////////////////////////////CLASS FOR THE GENETIC ALGORITHM//////////////////////////////////////////

class Individual{
public:
    //constructors
    Individual() = delete;
    Individual( int nNumberOfParameters ) :
        parameters( nNumberOfParameters ),
        fValue( std::numeric_limits<double>::max() )
    {}
    Individual( const spectral::array& pparameters ) :
        parameters( pparameters ),
        fValue( std::numeric_limits<double>::max() )
    {}
    Individual( const Individual& otherIndividual ) :
        parameters( otherIndividual.parameters ),
        fValue( otherIndividual.fValue )
    {}

    //methods
    std::pair<Individual, Individual> crossOver( const Individual& otherIndividual,
                                                 int pointOfCrossOver ) const {
        assert( parameters.size() && otherIndividual.parameters.size() &&
            "Individual::crossOver(): Either operands have zero parameters.") ;
        Individual child1( parameters.size() ), child2( parameters.size() );
        for( int iParameter = 0; iParameter < parameters.size(); ++iParameter ){
            if( iParameter < pointOfCrossOver ){
                child1.parameters[iParameter] = parameters[iParameter];
                child2.parameters[iParameter] = otherIndividual.parameters[iParameter];
            } else {
                child1.parameters[iParameter] = otherIndividual.parameters[iParameter];
                child2.parameters[iParameter] = parameters[iParameter];
            }
        }
        return { child1, child2 };
    }
    void mutate( double mutationRate,
                 const spectral::array& lowBoundaries,
                 const spectral::array& highBoundaries ){
        //sanity checks
        if( lowBoundaries.size() != parameters.size() ||
            highBoundaries.size() != parameters.size() ){
            QString message ("Individual::mutate(): Either low boundary (n=");
            message.append( QString::number( lowBoundaries.size() ) );
            message.append( ") or the high boundary (n=" );
            message.append( QString::number( highBoundaries.size() ) );
            message.append( ") have a different number of elements than the parameters member (n=" );
            message.append( QString::number( parameters.size() ) );
            message.append( "). Operation canceled.") ;
            QMessageBox::critical( nullptr, "Error", message);
            return;
        }
        //compute the mutation probability for a single gene (parameter)
        double probOfMutation = 1.0 / parameters.size() * mutationRate;
        //traverse all genes (parameters)
        for( int iPar = 0.0; iPar < parameters.size(); ++iPar ){
            //draw a value between 0.0 and 1.0 from an uniform distribution
            double p = std::rand() / (double)RAND_MAX;
            //if a mutation is due...
            if( p < probOfMutation ) {
                //perform mutation by randomly sorting a value within the domain.
                double LO = lowBoundaries[iPar];
                double HI = highBoundaries[iPar];
                parameters[iPar] = LO + std::rand() / (RAND_MAX/(HI-LO));
            }
        }
    }

    //member variables
    spectral::array parameters;
    double fValue;

    //operators
    Individual& operator=( const Individual& otherIndividual ){
        parameters = otherIndividual.parameters;
        fValue = otherIndividual.fValue;
        return *this;
    }
    bool operator<( const Individual& otherIndividual ) const {
        return fValue < otherIndividual.fValue;
    }

};
typedef Individual Solution; //make a synonym just for code readbility
/////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * The code for multithreaded evaluation of the objective function for a range of individuals (a set of variogam parameters)
 * in the Genetic Algorithm.
 * @param autoVarFitRef Reference to the AutomaticVarariogramFitting object so its objective function can be called.
 * @param m Number of variogram nested structures.
 * @param iIndividual_initial First individual index to process.
 * @param iIndividual_final Last individual index to process.  If final_i==initial_i, this thread processes only one individual.
 * @param inputGrid The grid object with grid geometry.
 * @param inputData The grid input data.
 * OUTPUT PARAMETER:
 * @param population The set of individuals to receive the evaluation of the objective function.
 */void taskEvaluateObjetiveInRangeOfIndividualsForGenetic(
        const AutomaticVariogramFitting& autoVarFitRef,
        int iIndividual_initial,
        int iIndividual_final,
        int m,
        const IJAbstractCartesianGrid& inputGrid,
        const spectral::array& inputData,
        std::vector< Individual >& population  //-->output parameter
        ) {
    for( int iInd = iIndividual_initial; iInd <= iIndividual_final; ++iInd ){
        Individual& ind = population[iInd];
        ind.fValue = autoVarFitRef.objectiveFunction( inputGrid, inputData, ind.parameters, m );
    }
}


AutomaticVariogramFitting::AutomaticVariogramFitting( Attribute *at ) :
    m_at( at ),
    m_fastVarmapMethod( FastVarmapMethod::VARMAP_WITH_FIM ),
    m_objectiveFunctionType( ObjectiveFunctionType::BASED_ON_FIM )
{
    assert( at && "AutomaticVariogramFitting::AutomaticVariogramFitting(): attribute cannot be null.");

    //display the selected input data names
    File* dataFile = at->getContainingFile();

    // get the input data's grid
    m_cg = dynamic_cast<CartesianGrid*>( dataFile );
    assert( m_cg && "AutomaticVariogramFitting::AutomaticVariogramFitting(): only attributes from CartesianGrids can be used.");

}

AutomaticVariogramFitting::~AutomaticVariogramFitting()
{
}

void AutomaticVariogramFitting::setFastVarmapMethod(FastVarmapMethod fastVarmapMethod)
{
    m_fastVarmapMethod = fastVarmapMethod;
}

spectral::array AutomaticVariogramFitting::computeVarmap() const
{
    //Get input data as a raw data array
    spectral::arrayPtr inputData( m_cg->createSpectralArray( m_at->getAttributeGEOEASgivenIndex()-1 ) ) ;

    if( m_fastVarmapMethod == FastVarmapMethod::VARMAP_WITH_FIM )
        return Util::getVarmapFIM( *inputData );
    else
        return Util::getVarmapSpectral( *inputData );
}

spectral::array AutomaticVariogramFitting::generateVariographicSurface(
                                            const IJAbstractCartesianGrid& gridWithGeometry,
                                            const spectral::array &vectorOfParameters,
                                            const int m ) const {
    //get grid parameters
    int nI = gridWithGeometry.getNI();
    int nJ = gridWithGeometry.getNJ();
    int nK = gridWithGeometry.getNK();
    //create a grid compatible with the input varmap
    spectral::array variographicSurface( nI, nJ, nK, 0.0 );
    //for each variogram structure
    for( int i = 0, iStructure = 0; iStructure < m; ++iStructure ){
        //create a variographic structure
        IJVariographicStructure2D varEllip(0.0, 0.0, 0.0, 0.0);
        //for each variographic parameter
        for( int iPar = 0; iPar < IJVariographicStructure2D::getNumberOfParameters(); ++iPar, ++i ){
            //set it to the variographic ellipse
            varEllip.setParameter( iPar, vectorOfParameters[i] );
        }
        //make the variographic surface
        varEllip.addContributionToModelGrid( gridWithGeometry,
                                             variographicSurface,
                                             IJVariogramPermissiveModel::SPHERIC,
                                             false );
    }
    return variographicSurface;
}

double AutomaticVariogramFitting::objectiveFunction( const IJAbstractCartesianGrid& gridWithGeometry,
           const spectral::array &inputGridData,
           const spectral::array &vectorOfParameters,
           const int m ) const  {
    switch( m_objectiveFunctionType ){
        case ObjectiveFunctionType::BASED_ON_FIM: return objectiveFunctionFIM( gridWithGeometry,
                                                                               inputGridData,
                                                                               vectorOfParameters,
                                                                               m);
        case ObjectiveFunctionType::BASED_ON_VARFIT: return objectiveFunctionVARFIT( gridWithGeometry,
                                                                                     inputGridData,
                                                                                     vectorOfParameters,
                                                                                     m);
        default: return -999.0;
    }
}

double AutomaticVariogramFitting::objectiveFunctionVARFIT( const IJAbstractCartesianGrid& gridWithGeometry,
           const spectral::array &inputGridData,
           const spectral::array &vectorOfParameters,
           const int m ) const  {
    std::unique_lock<std::mutex> objectiveFunctionlock ( myMutexObjectiveFunction, std::defer_lock );

    //get grid parameters
    int nI = gridWithGeometry.getNI();
    int nJ = gridWithGeometry.getNJ();
    int nK = gridWithGeometry.getNK();


    //Get input's varmap (this only needs to be redone when the input variable changes)
    static spectral::array inputVarmap; //this is initialized when the program loads
    {
        static Attribute* currentAttribute = nullptr; //this is initialized when the program loads
        if( inputVarmap.data().empty() || currentAttribute != m_at ){
            objectiveFunctionlock.lock(); //this lock prevents two threads from populating the static (global) variable at once.
            if( inputVarmap.data().empty() || currentAttribute != m_at ){ //repeat the outer if() to avoid unnecessary recomputing varmap
                                                                          //conversely, the role of the outer if is to wrap the lock so the
                                                                          //threads don't queue.
                Application::instance()->logInfo("AutomaticVariogramFitting::objectiveFunction(): computing varmap.");
                inputVarmap = computeVarmap();
                currentAttribute = m_at;
            }
            objectiveFunctionlock.unlock();
        }
    }

    //generate the variogram model surface from the parameters
    spectral::array theoreticalVariographicSurface = generateVariographicSurface( gridWithGeometry,
                                                                       vectorOfParameters,
                                                                       m );

    double meanSampleSpacing = ( gridWithGeometry.getCellSizeI() +
                                 gridWithGeometry.getCellSizeJ() +
                                 gridWithGeometry.getCellSizeK() ) / 3.0;

    //compute the weights for the experimental varmap points (this only needs to be redone when the input variable changes)
    static spectral::array weights; //this is initialized when the program loads
    {
        static Attribute* currentAttribute2 = nullptr; //this is initialized when the program loads
        if( weights.data().empty() || currentAttribute2 != m_at ){
            objectiveFunctionlock.lock(); //this lock prevents two threads from populating the static (global) variable at once.
            spectral::array weights_tmp = spectral::array( nI, nJ, nK, 0.0 );
            if( weights.data().empty() || currentAttribute2 != m_at ){ //repeat the outer if() to avoid unnecessary recomputing the weights
                                                                      //conversely, the role of the outer if is to wrap the lock so the
                                                                      //threads don't queue.
                Application::instance()->logInfo("AutomaticVariogramFitting::objectiveFunction(): computing varmap weights.");
                currentAttribute2 = m_at;

                //get the grid center location
                SpatialLocation gridCenter = m_cg->getCenter();
                double x, y, z;

                //compute the weights as a function of inverse distance from the center of the map
                //which correspond to hx=0, hy=0 of the variogram.
                {
                    for( int k = 0; k < nK; ++k )
                        for( int j = 0; j < nJ; ++j )
                            for( int i = 0; i < nI; ++i ) {
                                m_cg->getCellLocation( i, j, k, x, y, z );
                                double d = gridCenter.distanceTo( x, y, z );
                                if( d < 0.0001 ){ //if the separation is too small (results in large weight), this usually happens at the center
                                    weights_tmp( i, j, k ) = 0.0; //takes the opportunity to save the inv. lag distance beforehand
                                }else{
                                    weights_tmp( i, j, k ) = 1.0/d / ( 6.28*d/meanSampleSpacing ); //takes the opportunity to save the inv. lag distance beforehand
                                }
                            }
                }

                //after all weights have been calculated, set them to the static variable
                weights = weights_tmp;
            }

            objectiveFunctionlock.unlock();
        }
    }

    //compute the objective function metric
    double sum = 0.0;
    for( int k = 0; k < nK; ++k )
        for( int j = 0; j < nJ; ++j )
            for( int i = 0; i < nI; ++i ) {
                double diff = theoreticalVariographicSurface( i, j, k ) - inputVarmap( i, j, k );
                sum += weights(i, j, k) * diff*diff;
            }

    // Finally, return the objective function value.
    return sum;
}

double AutomaticVariogramFitting::objectiveFunctionFIM( const IJAbstractCartesianGrid& gridWithGeometry,
           const spectral::array &inputGridData,
           const spectral::array &vectorOfParameters,
           const int m ) const  {
    std::unique_lock<std::mutex> objectiveFunctionlock ( myMutexObjectiveFunction, std::defer_lock );

    //get grid parameters
    int nI = gridWithGeometry.getNI();
    int nJ = gridWithGeometry.getNJ();
    int nK = gridWithGeometry.getNK();

    //compute the FFT phase map of the input data
    static spectral::array inputFFTphases; //this is initialized when the program loads
    {
        static Attribute* currentAttribute3 = nullptr; //this is initialized when the program loads
        if( inputFFTphases.data().empty() || currentAttribute3 != m_at ){
            objectiveFunctionlock.lock(); //this lock prevents two threads from populating the static (global) variable at once.
            if( inputFFTphases.data().empty() || currentAttribute3 != m_at ){ //repeat the outer if() to avoid unnecessary recomputing the weights
                                                                              //conversely, the role of the outer if is to wrap the lock so the
                                                                              //threads don't queue.
                inputFFTphases = getInputPhaseMap();
                Application::instance()->logInfo("AutomaticVariogramFitting::objectiveFunction(): computing input's FFT phase map.");
                currentAttribute3 = m_at;
            }

            objectiveFunctionlock.unlock();
        }
    }

    //generate the variogram model surface from the parameters
    spectral::array theoreticalVariographicSurface = generateVariographicSurface( gridWithGeometry,
                                                                       vectorOfParameters,
                                                                       m );

    //generate the map from the theoretical variographic structure
    spectral::array mapFromTheoreticalVariographicStructure =
            computeFIM( theoreticalVariographicSurface, inputFFTphases );

    //compute the objective function metric
    double sum = 0.0;
    for( int k = 0; k < nK; ++k )
        for( int j = 0; j < nJ; ++j )
            for( int i = 0; i < nI; ++i ) {
                double diff = mapFromTheoreticalVariographicStructure( i, j, k ) - inputGridData( i, j, k );
                sum += diff*diff;
            }

    // Finally, return the objective function value.
    return sum;
}

spectral::array AutomaticVariogramFitting::getInputPhaseMap() const
{
    std::unique_lock<std::mutex> FFTWlock ( myMutexFFTW, std::defer_lock );
    spectral::arrayPtr inputGridData( m_cg->createSpectralArray( m_at->getAttributeGEOEASgivenIndex()-1 ) );
    spectral::array tmp( *inputGridData );
    spectral::complex_array inputFFT;
    FFTWlock.lock();                //
    spectral::foward( inputFFT, tmp ); // FFTW crashes when called concurrently
    FFTWlock.unlock();              //
    spectral::complex_array inputFFTpolar = spectral::to_polar_form( inputFFT );
    return spectral::imag( inputFFTpolar );
}

void AutomaticVariogramFitting::onSaveAResult(spectral::array *result)
{
    //user enters the name for the new variable
    bool ok;
    QString new_var_name = QInputDialog::getText(Application::instance()->getMainWindow(),
                                             "Create new variable in " + m_cg->getName(),
                                             "New variable name:", QLineEdit::Normal,
                                             "Nth_varmap_or_structure_of_" + m_at->getName(), &ok );

    //abort if the user cancels the input box
    if ( !ok || new_var_name.isEmpty() ){
        return;
    }

    //append the data as a new attribute to the destination grid.
    m_cg->append( new_var_name, *result );
}

void AutomaticVariogramFitting::displayGrids(const std::vector<spectral::array> &grids,
                                         const std::vector<std::string> &titles,
                                         const std::vector<bool> & shiftByHalves,
                                         bool modal ) const
{
    //Create the structure to store the variographic structure factors
    SVDFactorTree * factorTree = new SVDFactorTree( 0.0 ); //the split factor of 0.0 has no special meaning here
    //Populate the factor container with the structure factors.
    std::vector< spectral::array >::const_iterator it = grids.begin();
    std::vector< std::string >::const_iterator itTitles = titles.begin();
    std::vector< bool >::const_iterator itShiftByHalves = shiftByHalves.begin();
    for(int i = 1; it != grids.end(); ++it, ++i, ++itTitles, ++itShiftByHalves){
        //make a local copy of the structure map data
        spectral::array structureMapDataCopy;
        if( *itShiftByHalves )
            structureMapDataCopy = spectral::shiftByHalf( *it );
        else
            structureMapDataCopy = spectral::array( *it );
        //Create a displayble object from the structure factor data
        //This pointer will be managed by the SVDFactorTree object.
        SVDFactor* structureFactor = new SVDFactor( std::move(structureMapDataCopy), i, 1/(grids.size()),
                                           0, 0, 0, 1, 1, 1, 0.0);
        //Declare it as a structure factor (decomposable, not fundamental)
        structureFactor->setType( SVDFactorType::GEOLOGICAL );
        structureFactor->setCustomName( QString( (*itTitles).c_str() ) );
        //add the displayable object to the factor tree (container)
        factorTree->addFirstLevelFactor( structureFactor );
    }
    //use the SVD analysis dialog to display the structure factors.
    //NOTE: do not use heap to allocate the dialog, unless you remove the Qt::WA_DeleteOnClose behavior of the dialog.
    SVDAnalysisDialog* svdad = new SVDAnalysisDialog( Application::instance()->getMainWindow() );
    svdad->setWindowTitle("Grids display: right-click on a grid to save it to the data set.");
    svdad->setTree( factorTree );
    svdad->setDeleteTreeOnClose( true ); //the three and all data it contains will be deleted on dialog close
    svdad->hideAnalysisButtons(); //we are not doing SVD analysis
    connect( svdad, SIGNAL(sumOfFactorsComputed(spectral::array*)),
             this,  SLOT(onSaveAResult(spectral::array*)) );
    if( modal )
        svdad->exec();
    else
        svdad->show();
}

spectral::array AutomaticVariogramFitting::computeFIM( const spectral::array &gridWithCovariance,
                                                   const spectral::array &gridWithFFTphases ) const
{
    std::unique_lock<std::mutex> FFTWlock ( myMutexFFTW, std::defer_lock );

    //get grid dimensions
    size_t nI = gridWithCovariance.M();
    size_t nJ = gridWithCovariance.N();
    size_t nK = gridWithCovariance.K();

    //prepare the result
    spectral::array result( nI, nJ, nK, 0.0 );

    //de-centralize de covariance values (h=0 goes to the corners of the grid)
    // the multiplication by ( nI * nJ * nK ) is to keep simmetry with the division by the same value further down.
    spectral::array covarianceDecentralized = spectral::shiftByHalf( gridWithCovariance ) * static_cast<double>( nI * nJ * nK );

    //compute FFT of the variographic surface (into polar form)
    spectral::complex_array variographicSurfaceFFT( nI, nJ, nK );
    FFTWlock.lock();                                                     //
    spectral::foward( variographicSurfaceFFT, covarianceDecentralized);  // FFTW crashes when called concurrently
    FFTWlock.unlock();                                                   //

    //convert the FFT result (as complex numbers in a + bi form) to polar form (amplitudes and phases)
    spectral::complex_array variographicSurfaceFFTpolar = spectral::to_polar_form( variographicSurfaceFFT );

    //get the FFT amplitudes of the covariance values to get the spectral density
    spectral::array spectralDensity = spectral::real( variographicSurfaceFFTpolar );

    //get the square root of the spectral density to get the FFT amplitude spectrum of the resulting map
    spectral::array mapFFTamplitudes = spectralDensity.sqrt();

    //convert the FFT amplitudes and phases (passad as parameter) of the future result to rectangular form (a + bi)
    spectral::complex_array mapFFTpolar = spectral::to_complex_array(
                                                   mapFFTamplitudes,
                                                   gridWithFFTphases
                                                );
    spectral::complex_array mapFFT = spectral::to_rectangular_form( mapFFTpolar );

    //compute the reverse FFT to get "factorial kriging"
    FFTWlock.lock();                      //
    spectral::backward( result, mapFFT ); // FFTW crashes when called concurrently
    FFTWlock.unlock();                    //

    //fftw3's reverse FFT requires that the values of output be divided by the number of cells
    result = result / static_cast<double>( nI * nJ * nK );

    //return the result
    return result;
}

void AutomaticVariogramFitting::showObjectiveFunctionEvolution() const
{
    //load the x,y data for the chart
    QtCharts::QLineSeries *chartSeries = new QtCharts::QLineSeries();
    double max = std::numeric_limits<double>::lowest();
    for(uint i = 0; i < s_objectiveFunctionValues.size(); ++i){
        chartSeries->append( i+1, s_objectiveFunctionValues[i] );
        if( s_objectiveFunctionValues[i] > max )
            max = s_objectiveFunctionValues[i];
    }

    //create a new chart object
    QtCharts::QChart *objFuncValuesChart = new QtCharts::QChart();
    {
        objFuncValuesChart->addSeries( chartSeries );
        objFuncValuesChart->axisX( chartSeries );

        QtCharts::QValueAxis* axisX = new QtCharts::QValueAxis();
        axisX->setLabelFormat("%i");
        objFuncValuesChart->setAxisX(axisX, chartSeries);

        QtCharts::QValueAxis *axisY = new QtCharts::QValueAxis();
        axisY->setLabelFormat("%3.2f");
        axisY->setRange( 0.0, max );
        objFuncValuesChart->setAxisY(axisY, chartSeries);

        objFuncValuesChart->legend()->hide();
    }

    //create the chart dialog
    EmptyDialog* ed = new EmptyDialog( Application::instance()->getMainWindow() );
    QtCharts::QChartView* chartView = new QtCharts::QChartView( objFuncValuesChart );
    ed->addWidget( chartView );
    ed->setWindowTitle( "Objective function with iterations." );
    ed->show();
}

void AutomaticVariogramFitting::displayResults( const std::vector<IJVariographicStructure2D> &variogramStructures,
                                            const spectral::array& fftPhaseMapOfInput,
                                            const spectral::array& varmapOfInput,
                                            bool modal ) const
{
    int m = variogramStructures.size();
    //Apply the principle of the Fourier Integral Method
    //use a variographic map as the magnitudes and the FFT phases of
    //the original data to a reverse FFT in polar form to achieve a
    //Factorial Kriging-like separation
    std::vector< spectral::array > maps;
    std::vector< std::string > titles;
    std::vector< bool > shiftFlags;
    uint nI = m_cg->getNI();
    uint nJ = m_cg->getNJ();
    uint nK = m_cg->getNK();

    // Get the input grid data
    spectral::arrayPtr inputData( m_cg->createSpectralArray( m_at->getAttributeGEOEASgivenIndex()-1 ) ) ;

    // Prepare the display of the variogram model surface (all nested structures added up)
    spectral::array variograficSurface( nI, nJ, nK, 0.0 );

    // The sum of the indidual maps corresponding to each structure
    spectral::array sumOfStructures( nI, nJ, nK, 0.0 );

    for( int iStructure = 0; iStructure < m; ++iStructure ) {
        //compute the theoretical varmap for one structure
        spectral::array oneStructureVarmap( nI, nJ, nK, 0.0 );
        variogramStructures[iStructure].addContributionToModelGrid( *m_cg,
                                                                     oneStructureVarmap,
                                                                     IJVariogramPermissiveModel::SPHERIC,
                                                                     true );

        //build up the complete model surface (all nested structures added up)
        variograficSurface += oneStructureVarmap;

        //collect the theoretical varmap for display
        //oneStructureVarmap = oneStructureVarmap.max() - oneStructureVarmap; // correlogram -> variogram
        //display inverted so it appears with 0.0 at center (h=0)
        maps.push_back( oneStructureVarmap.max() - oneStructureVarmap );
        QString structureDesc = "Str. " + QString::number( iStructure ) + ": "
                                "Sph "
                                "cc="   + Util::formatToDecimalPlaces( variogramStructures[iStructure].contribution, 3 ) + ";\n "
                                "axes=" + Util::formatToDecimalPlaces( variogramStructures[iStructure].range, 3 ) +
                                " X " + Util::formatToDecimalPlaces( variogramStructures[iStructure].range * variogramStructures[iStructure].rangeRatio, 3 ) + "; "
                                "az="   + Util::formatToDecimalPlaces( Util::radiansToHalfAzimuth( variogramStructures[iStructure].azimuth, true ), 3 ) + "; ";
        titles.push_back( structureDesc.toStdString() ) ;
        shiftFlags.push_back( false );

        //compute FIM to obtain the map from a variographic structure
        spectral::array oneStructure( nI, nJ, nK, 0.0 );
        oneStructure = computeFIM( oneStructureVarmap, fftPhaseMapOfInput );

        //accumulate the structures
        sumOfStructures += oneStructure;

        //collect the "FK factor"
        maps.push_back( oneStructure );
        titles.push_back( QString( "Map " + QString::number( iStructure ) ).toStdString() );
        shiftFlags.push_back( false );
    }

    // Collect the data to display the complete model surface (all nested structures added up)
    variograficSurface = variograficSurface.max() - variograficSurface;
    maps.push_back( variograficSurface );
    titles.push_back( QString( "Variogram model surface" ).toStdString() );
    shiftFlags.push_back( false );

    // Prepare the display the experimental varmap of the input
    maps.push_back( varmapOfInput );
    titles.push_back( QString( "Varmap of input" ).toStdString() );
    shiftFlags.push_back( false );

    // Prepare the display of the difference experimental - model
    spectral::array diffVarmapAndModel = varmapOfInput - variograficSurface;
    maps.push_back( diffVarmapAndModel );
    titles.push_back( QString( "Difference (variogram)" ).toStdString() );
    shiftFlags.push_back( false );

    // Display the input data
    maps.push_back( *inputData );
    titles.push_back( QString( "Original grid" ).toStdString() );
    shiftFlags.push_back( false );

    // Get the objective function value corresponding to the fitted variogram model.
    double objectiveFunctionValue = evaluateModel( variogramStructures );

    // Display the sum of factors obtained with the nested structures
    maps.push_back( sumOfStructures );
    titles.push_back( QString( "Result of the model (F=" +
                               Util::formatToDecimalPlaces( objectiveFunctionValue, 3 ) ).toStdString() + ")" );
    shiftFlags.push_back( false );

    //compute FIM to obtain the map from the residual varmap - variogram model
    spectral::array mapFromDiffVarmapAndModel( nI, nJ, nK, 0.0 );
    mapFromDiffVarmapAndModel = computeFIM( diffVarmapAndModel, fftPhaseMapOfInput );
    maps.push_back( mapFromDiffVarmapAndModel );
    titles.push_back( QString( "Result of diff. varmap - model" ).toStdString() );
    shiftFlags.push_back( false );

    // Prepare the display of the difference original data - sum of factors
    maps.push_back( *inputData - sumOfStructures );
    titles.push_back( QString( "Difference (map)" ).toStdString() );
    shiftFlags.push_back( false );

    // Display all the grids in a dialog
    displayGrids( maps, titles, shiftFlags, modal );
}

void AutomaticVariogramFitting::initDomainAndParameters( const spectral::array& inputVarmap,
                                                     int m,
                                                     VariogramParametersDomain &domain,
                                                     spectral::array &vw,
                                                     spectral::array &L_wMin,
                                                     spectral::array &L_wMax,
                                                     std::vector<IJVariographicStructure2D>& variogramStructures) const
{
    //define the domain
    double minCellSize = std::min( m_cg->getCellSizeI(), m_cg->getCellSizeJ() );
    double minAxis         = minCellSize              ; double maxAxis         = m_cg->getDiagonalLength() / 2.0;
    double minRatio        = 0.001                    ; double maxRatio        = 1.0;
    double minAzimuth      = 0.0                      ; double maxAzimuth      = ImageJockeyUtils::PI;
    double minContribution = inputVarmap.max() / 100.0; double maxContribution = inputVarmap.max();

    //create the nested structures wanted by the user
    //the parameters are initialized near in the center of the domain
    for( int i = 0; i < m; ++i )
        variogramStructures.push_back( IJVariographicStructure2D (  ( maxAxis         + minAxis         ) / 2.0,
                                                                    ( maxRatio        + minRatio        ) / 2.0,
                                                                    ( minAzimuth      + maxAzimuth      ) / 2.0,
                                                                    ( maxContribution - minContribution ) / m ) );

    //Initialize the vector of all variographic parameters [w]=[axis0,ratio0,az0,cc0,axis1,ratio1,...]
    //this vector is used in optimization steps .
    // the starting values are not particuarly important.
    vw = spectral::array( (spectral::index)( m * IJVariographicStructure2D::getNumberOfParameters() ) );
    for( int iLinearIndex = 0, iStructure = 0; iStructure < m; ++iStructure )
        for( int iIndexInStructure = 0; iIndexInStructure < IJVariographicStructure2D::getNumberOfParameters(); ++iIndexInStructure, ++iLinearIndex )
            vw[iLinearIndex] = variogramStructures[iStructure].getParameter( iIndexInStructure );

    //Minimum value allowed for the parameters w
    //(see min* variables further up). DOMAIN CONSTRAINT
    L_wMin = spectral::array( vw.size(), 0.0 );
    for(int i = 0, iStructure = 0; iStructure < m; ++iStructure )
        for( int iPar = 0; iPar < IJVariographicStructure2D::getNumberOfParameters(); ++iPar, ++i )
            switch( iPar ){
            case 0: L_wMin[i] = minAxis;         break;
            case 1: L_wMin[i] = minRatio;        break;
            case 2: L_wMin[i] = minAzimuth;      break;
            case 3: L_wMin[i] = minContribution; break;
            }

    //Maximum value allowed for the parameters w
    //(see max* variables further up). DOMAIN CONSTRAINT
    L_wMax = spectral::array( vw.size(), 1.0 );
    for(int i = 0, iStructure = 0; iStructure < m; ++iStructure )
        for( int iPar = 0; iPar < IJVariographicStructure2D::getNumberOfParameters(); ++iPar, ++i )
            switch( iPar ){
            case 0: L_wMax[i] = maxAxis;         break;
            case 1: L_wMax[i] = maxRatio;        break;
            case 2: L_wMax[i] = maxAzimuth;      break;
            case 3: L_wMax[i] = maxContribution; break;
            }

    // Return the domain
    domain.min.range        = minAxis;
    domain.min.rangeRatio   = minRatio;
    domain.min.azimuth      = minAzimuth;
    domain.min.contribution = minContribution;
    domain.max.range        = maxAxis;
    domain.max.rangeRatio   = maxRatio;
    domain.max.azimuth      = maxAzimuth;
    domain.max.contribution = maxContribution;
}

std::vector<IJVariographicStructure2D> AutomaticVariogramFitting::processWithSAandGD(unsigned int nThreads,
        int m,
        unsigned seed,
        double f_Tinitial,
        double f_Tfinal,
        int i_kmax,
        double f_factorSearch,
        int maxNumberOfOptimizationSteps,
        double epsilon,
        double initialAlpha,
        double maxNumberOfAlphaReductionSteps,
        double convergenceCriterion,
        bool openResultsDialog) const
{
    //clear the collected objective function values.
    s_objectiveFunctionValues.clear();

    // Intialize the random number generator with the same seed
    std::srand (seed);

    //================================== PREPARE OPTIMIZATION =============================================

    // Get the input data as a spectral::array object
    spectral::arrayPtr inputData( m_cg->createSpectralArray( m_at->getAttributeGEOEASgivenIndex() - 1 ) );

    // Get experimental variogram (varmap) to be used for comparison with the variogram model
    // in the objective function.
    spectral::array inputVarmap = computeVarmap();

    //Get input's FFT phase map
    spectral::array inputFFTimagPhase = getInputPhaseMap();

    //Initialize the optimization domain (boundary conditions) and
    //the sets of variogram paramaters (both linear and structured)
    VariogramParametersDomain domain;
    spectral::array vw;
    spectral::array L_wMin;
    spectral::array L_wMax;
    std::vector< IJVariographicStructure2D > variogramStructures;
    initDomainAndParameters( inputVarmap,
                             m,
                             domain,
                             vw,
                             L_wMin,
                             L_wMax,
                             variogramStructures );

    //-------------------------------------------------------------------------------------------------------------
    //-------------------------SIMULATED ANNEALING TO INITIALIZE THE PARAMETERS [w] NEAR A GLOBAL MINIMUM------------
    //---------------------------------------------------------------------------------------------------------------
    {

        /* This lambda returns the current “temperature” of the system.  It yields a log curve that decays as the step number increase.
           The initial temperature plays an important role: curve starting with 5.000 is steeper than another that starts with 1.000.
           This means the the lower the temperature, the more linear the temperature decreases.
           i_stepNumber: the current step number of the annealing process ( 0 = first ). */
        auto temperature = [=](int i_stepNumber) {
                  return f_Tinitial * std::exp( -i_stepNumber / (double)1000 * (1.5 * std::log10( f_Tinitial ) ) );
        };

        /* This lambda returns the probability of acceptance of the energy state for the next iteration.
          This allows acceptance of higher values to break free from local minima.
          f_eCurrent: current energy of the system.
          f_eNew: energy level of the next step.
          f_T: current “temperature” of the system.*/
        auto probAcceptance = [=]( double f_eCurrent, double f_eNewLocal, double f_T ) {
           //If the new state is more energetic, calculates a probability of acceptance
           //which is as high as the current “temperature” of the process.  The “temperature”
           //diminishes with iterations.
           if ( f_eNewLocal > f_eCurrent )
              return ( f_T - f_Tfinal ) / ( f_Tinitial - f_Tfinal );
           //If the new state is less energetic, the probability of acceptance is 100% (natural search for minima).
           else
              return 1.0 - ( f_T - f_Tfinal ) / ( f_Tinitial - f_Tfinal );
        };

        //Get the number of parameters.
        int i_nPar = vw.size();

        //Make a copy of the initial state (parameter set).
        spectral::array L_wCurrent( vw );

        //The parameters variations (maxes - mins)
        spectral::array L_wDelta = L_wMax - L_wMin;

        //Give visual feedback to the user (processing may take a while)
        QProgressDialog progressDialog;
        progressDialog.setRange(0, i_kmax);
        progressDialog.setValue( 0 );
        progressDialog.show();
        progressDialog.setLabelText("Simulated Annealing in progress...");
        QCoreApplication::processEvents();

        //...................Main annealing loop...................
        double f_eNew = std::numeric_limits<double>::max();
        double f_lowestEnergyFound = std::numeric_limits<double>::max();
        spectral::array L_wOfLowestEnergyFound;
        int k = 0;
        for( ; k < i_kmax; ++k ){
            //Get current temperature.
            double f_T = temperature( k );
            //Quit if temperature is lower than the minimum annealing temperature.
            if( f_T < f_Tfinal )
                /*break*/;
            //Randomly searches for a neighboring state with respect to current state.
            spectral::array L_wNew(L_wCurrent);
            for( int i = 0; i < i_nPar; ++i ){ //for each parameter
               //Ensures that the values randomly obtained are inside the domain.
               double f_tmp = 0.0;
               while( true ){
                  double LO = L_wCurrent[i] - (f_factorSearch * L_wDelta[i]);
                  double HI = L_wCurrent[i] + (f_factorSearch * L_wDelta[i]);
                  f_tmp = LO + std::rand() / (RAND_MAX/(HI-LO)) ;
                  if ( f_tmp >= L_wMin[i] && f_tmp <= L_wMax[i] )
                     break;
               }
               //Updates the parameter value.
               L_wNew[i] = f_tmp;
            }
            //Computes the “energy” of the current state (set of parameters).
            //The “energy” in this case is how different the image as given the parameters is with respect
            //the data grid, considered the reference image.
            double f_eCurrent = objectiveFunction( *m_cg, *inputData, L_wCurrent, m );

            //Computes the “energy” of the neighboring state.
            f_eNew = objectiveFunction( *m_cg, *inputData, L_wNew, m );
            //Changes states stochastically.  There is a probability of acceptance of a more energetic state so
            //the optimization search starts near the global minimum and is not trapped in local minima (hopefully).
            double f_probMov = probAcceptance( f_eCurrent, f_eNew, f_T );
            if( f_probMov >= ( (double)std::rand() / RAND_MAX ) ) {//draws a value between 0.0 and 1.0
                L_wCurrent = L_wNew; //replaces the current state with the neighboring random state
                 //Application::instance()->logInfo("  moved to energy level " + QString::number( f_eNew ));
                //if the energy is the record low, store it, just in case the SA loop ends without converging.
                if( f_eNew < f_lowestEnergyFound ){
                    f_lowestEnergyFound = f_eNew;
                    L_wOfLowestEnergyFound = spectral::array( L_wCurrent );
                }
            }

            //collect the interation's objective function value
            s_objectiveFunctionValues.push_back( f_eCurrent );

            //Let Qt repaint the GUI
            progressDialog.setValue( k );
            QCoreApplication::processEvents();
        } //.........................end of main annealing loop.................

        // Delivers the set of parameters near the global minimum (hopefully) for the Gradient Descent algorithm.
        // The SA loop may end in a higher energy state, so we return the lowest found in that case
        if( k == i_kmax && f_lowestEnergyFound < f_eNew )
            Application::instance()->logInfo( "SA completed by number of steps." );
        else
            Application::instance()->logInfo( "SA completed by reaching the lowest temperature." );
        vw = L_wOfLowestEnergyFound;
        Application::instance()->logInfo( "Using the state of lowest energy found (" + QString::number( f_lowestEnergyFound ) + ")" );
    }

    //---------------------------------------------------------------------------------------------------------
    //-------------------------------------- GRADIENT DESCENT PART --------------------------------------------
    //---------------------------------------------------------------------------------------------------------
    QProgressDialog progressDialog;
    progressDialog.setRange(0, maxNumberOfOptimizationSteps );
    progressDialog.show();
    progressDialog.setValue( 0 );
    progressDialog.setLabelText("Gradient Descent in progress...");
    int iOptStep = 0;
    for( ; iOptStep < maxNumberOfOptimizationSteps; ++iOptStep ){

        //Compute the gradient vector of objective function F with the current [w] parameters.
        spectral::array gradient( vw.size() );
        {

            //distribute the parameter indexes among the n-threads
            std::vector<int> parameterIndexBins[nThreads];
            int parameterIndex = 0;
            for( unsigned int iThread = 0; parameterIndex < vw.size(); ++parameterIndex, ++iThread)
                parameterIndexBins[ iThread % nThreads ].push_back( parameterIndex );

            //create and run the partial derivative calculation threads
            std::thread threads[nThreads];
            for( unsigned int iThread = 0; iThread < nThreads; ++iThread){
                threads[iThread] = std::thread( taskOnePartialDerivative,
                                                vw,
                                                parameterIndexBins[iThread],
                                                epsilon,
                                                m_cg,
                                                inputVarmap,
                                                m,
                                                this,
                                                &gradient);
            }

            //wait for the threads to finish.
            for( unsigned int iThread = 0; iThread < nThreads; ++iThread)
                threads[iThread].join();

        }

        //Update the system's parameters according to gradient descent.
        double currentF = std::numeric_limits<double>::max();
        double nextF = 1.0;
        {
            double alpha = initialAlpha;
            //halves alpha until we get a descent (current gradient vector may result in overshooting)
            int iAlphaReductionStep = 0;
            for( ; iAlphaReductionStep < maxNumberOfAlphaReductionSteps; ++iAlphaReductionStep ){
                spectral::array new_vw = vw - gradient * alpha;
                //Impose domain constraints to the parameters.
                for( int i = 0; i < new_vw.size(); ++i){
                    if( new_vw.d_[i] < L_wMin[i] )
                        new_vw.d_[i] = L_wMin[i];
                    if( new_vw.d_[i] > L_wMax[i] )
                        new_vw.d_[i] = L_wMax[i];
                }
                currentF = objectiveFunction( *m_cg, *inputData, vw,     m );
                nextF =    objectiveFunction( *m_cg, *inputData, new_vw, m );
                if( nextF < currentF ){
                    vw = new_vw;
                    break;
                }
                alpha /= 2.0;
            }
            if( iAlphaReductionStep == maxNumberOfAlphaReductionSteps )
                Application::instance()->logWarn( "WARNING: reached maximum alpha reduction steps." );
        }

        //collect the interation's objective function value
        s_objectiveFunctionValues.push_back( currentF );

        //Check the convergence criterion.
        double ratio = currentF / nextF;
        if( ratio  < (1.0 + convergenceCriterion) )
            break;

        Application::instance()->logInfo( "F(k)/F(k+1) ratio: " + QString::number( ratio ) );

        if( ! ( iOptStep % 10) ) { //to avoid excess calls to processEvents.
            progressDialog.setValue( iOptStep );
            QCoreApplication::processEvents();
        }
    } //--------GD's main loop-------
    progressDialog.hide();

    //Read the optimized variogram model parameters back to the variographic structures
    for( int i = 0, iStructure = 0; iStructure < m; ++iStructure )
        for( int iPar = 0; iPar < IJVariographicStructure2D::getNumberOfParameters(); ++iPar, ++i )
            variogramStructures[iStructure].setParameter( iPar, vw[i] );

    // Display the results in a window.
    if( openResultsDialog ){
        displayResults( variogramStructures, inputFFTimagPhase, inputVarmap, false );
        showObjectiveFunctionEvolution();
    }

    //return the fitted model
    return variogramStructures;
}

std::vector<IJVariographicStructure2D> AutomaticVariogramFitting::processWithLSRS(
        unsigned int nThreads,
        int m,
        unsigned seed,
        int maxNumberOfOptimizationSteps,
        double epsilon,
        int nStartingPoints,
        int nRestarts,
        bool openResultsDialog) const
{
    //clear the collected objective function values.
    s_objectiveFunctionValues.clear();

    //Intialize the random number generator with the same seed
    std::srand (seed);

    // Get the data objects.
    IJAbstractCartesianGrid* inputGrid = m_cg;
    IJAbstractVariable* variable = m_at;

    // Get the grid's dimensions.
    unsigned int nI = inputGrid->getNI();
    unsigned int nJ = inputGrid->getNJ();
    unsigned int nK = inputGrid->getNK();

    // Fetch data from the data source.
    inputGrid->dataWillBeRequested();

    //================================== PREPARE DATA ==========================

    // Get the input data as a spectral::array object
    spectral::arrayPtr inputData( inputGrid->createSpectralArray( variable->getIndexInParentGrid() ) );

    // Compute FFT phase map of input
    spectral::array inputFFTimagPhase = getInputPhaseMap();

    // Compute input's varmap
    spectral::array inputVarmap = computeVarmap();

    //Initialize the optimization domain (boundary conditions) and
    //the sets of variogram paramaters (both linear and structured)
    VariogramParametersDomain domain;
    spectral::array vw;
    spectral::array L_wMin;
    spectral::array L_wMax;
    std::vector< IJVariographicStructure2D > variogramStructures;
    initDomainAndParameters( inputVarmap,
                             m,
                             domain,
                             vw,
                             L_wMin,
                             L_wMax,
                             variogramStructures );

    //-------------------------------------------------------------------------------------------------------------
    //------------------------ THE MODIFIED LINE SEARCH ALGORITHM AS PROPOSED BY Grosan and Abraham (2009)---------
    //---------------------------A Novel Global Optimization Technique for High Dimensional Functions--------------
    //-------------------------------------------------------------------------------------------------------------

    QProgressDialog progressDialog;
    progressDialog.setRange(0, nRestarts * maxNumberOfOptimizationSteps );
    progressDialog.show();
    progressDialog.setValue( 0 );
    progressDialog.setLabelText("Line Search with Restart in progress...");

    //distribute as evenly as possible (load balance) the starting
    //points (by their indexes) amongst the threads.
    std::vector< std::pair< int, int > > startingPointsIndexesRanges =
            Util::generateSubRanges( 0, nStartingPoints-1, nThreads );

    //sanity check
    assert( startingPointsIndexesRanges.size() == nThreads && "AutomaticVariogramFitting::onDoWithLSRS(): "
                                                              "number of threads different from starting point index ranges. "
                                                              " This is likely a bug in Util::generateSubRanges() function." );

    //the line search restarting loop
    spectral::array vw_bestSolution( (spectral::index)( m * IJVariographicStructure2D::getNumberOfParameters() ) );
    for( int t = 0; t < nRestarts; ++t){

        //generate sarting points randomly within the domain
        // each starting point is a potential solution (set of parameters)
        std::vector< spectral::array > startingPoints;
        for( int iSP = 0; iSP < nStartingPoints; ++iSP ){
            spectral::array vw_StartingPoint( (spectral::index)( m * IJVariographicStructure2D::getNumberOfParameters() ) );
            for( int i = 0; i < vw_StartingPoint.size(); ++i ){
                double LO = L_wMin[i];
                double HI = L_wMax[i];
                vw_StartingPoint[i] = LO + std::rand() / (RAND_MAX/(HI-LO));
            }
            startingPoints.push_back( vw_StartingPoint );
        }

        //generate a random walk beforehand so the result is the same
        //independently of how threads execute.
        spectral::array randSequence( vw_bestSolution.size(),       //--> number of variogram parameters per solution
                                      nStartingPoints,              //--> number of solutions per optimization step
                                      maxNumberOfOptimizationSteps, //--> number of optimization steps
                                      0.0 );
        for( int k = 1; k <= maxNumberOfOptimizationSteps; ++k )
            for( int i = 0; i < nStartingPoints; ++i )
                for( int j = 0; j < vw_bestSolution.size(); ++j )
                    randSequence( j, i, k-1 ) = std::rand() / static_cast<double>( RAND_MAX );

        //----------------loop of line search algorithm----------------
        double fOfBestSolution = std::numeric_limits<double>::max();
        //for each step
        for( int k = 1; k <= maxNumberOfOptimizationSteps; ++k ){

            //create and start the threads.  Each thread moves a set of points along a set of lines.
            std::thread threads[nThreads];
            unsigned int iThread = 0;
            for( const std::pair< int, int >& startingPointsIndexesRange : startingPointsIndexesRanges ) {
                threads[iThread] = std::thread( taskMovePointAlongLineForLSRS,
                                                std::cref(*this),
                                                m,
                                                startingPointsIndexesRange.first,
                                                startingPointsIndexesRange.second,
                                                k,
                                                std::cref(domain),
                                                std::cref(L_wMax),
                                                std::cref(L_wMin),
                                                std::cref(*inputGrid),
                                                std::cref(*inputData),
                                                std::cref(randSequence),    //<-- INPUT PARAMETERS UP TO HERE
                                                std::ref(startingPoints),
                                                std::ref(fOfBestSolution),
                                                std::ref(vw_bestSolution)
                                                );                          //--> OUTPUT PARAMETERS UP TO HERE
                ++iThread;
            } //for each thread (ranges of starting points)

            //wait for the threads to finish.
            for( unsigned int iThread = 0; iThread < nThreads; ++iThread)
                threads[iThread].join();

            //collect the iteration's best objective function value
            s_objectiveFunctionValues.push_back( objectiveFunction( *inputGrid, *inputData, vw_bestSolution, m ) );

            progressDialog.setValue( t * maxNumberOfOptimizationSteps + k );
            QApplication::processEvents(); // let Qt update the UI

        } // search for best solution
        //---------------------------------------------------------------------------

        //for each parameter of the best solution
        for( int iParameter = 0; iParameter < vw.size(); ++iParameter ){
            //Make a set of parameters slightly shifted to the right (more positive) along one parameter.
            spectral::array vwFromRight = vw_bestSolution;
            vwFromRight(iParameter) = vw_bestSolution(iParameter) + epsilon;
            //Make a set of parameters slightly shifted to the left (more negative) along one parameter.
            spectral::array vwFromLeft = vw_bestSolution;
            vwFromLeft(iParameter) = vw_bestSolution(iParameter) - epsilon;
            //compute the partial derivative along one parameter
            double partialDerivative =  ( objectiveFunction( *inputGrid, *inputData, vwFromRight, m )
                                          -
                                          objectiveFunction( *inputGrid, *inputData, vwFromLeft, m ))
                                          /
                                          ( 2 * epsilon );
            //update the domain limits depending on the partial derivative result
            //this usually reduces the size of the domain so the next set of starting
            //points have a higher probability to be drawn near a global optimum.
            if( partialDerivative > 0 )
                L_wMax[ iParameter ] = vw_bestSolution[ iParameter ];
            else if( partialDerivative < 0 )
                L_wMin[ iParameter ] = vw_bestSolution[ iParameter ];
        } // reduce the domain to a smaller hyper volume around the suspected optimum

    } //restart loop
    progressDialog.hide();

    //Read the optimized variogram model parameters back to the variographic structures
    for( int i = 0, iStructure = 0; iStructure < m; ++iStructure )
        for( int iPar = 0; iPar < IJVariographicStructure2D::getNumberOfParameters(); ++iPar, ++i )
            variogramStructures[iStructure].setParameter( iPar, vw_bestSolution[i] );

    // Display the results in a window.
    if( openResultsDialog ){
        displayResults( variogramStructures, inputFFTimagPhase, inputVarmap, false );
        showObjectiveFunctionEvolution();
    }

    //return the fitted model
    return variogramStructures;
}

std::vector<IJVariographicStructure2D> AutomaticVariogramFitting::processWithPSO(
        int m,
        unsigned seed,
        int maxNumberOfOptimizationSteps,
        int nParticles,
        double intertia_weight,
        double acceleration_constant_1,
        double acceleration_constant_2,
        bool openResultsDialog) const
{
    //clear the collected objective function values.
    s_objectiveFunctionValues.clear();

    //Intialize the random number generator with the same seed
    std::srand (seed);

    // Get the data objects.
    IJAbstractCartesianGrid* inputGrid = m_cg;
    IJAbstractVariable* variable = m_at;

    // Get the grid's dimensions.
    unsigned int nI = inputGrid->getNI();
    unsigned int nJ = inputGrid->getNJ();
    unsigned int nK = inputGrid->getNK();

    // Fetch data from the data source.
    inputGrid->dataWillBeRequested();

    // Get the input data as a spectral::array object
    spectral::arrayPtr inputData( inputGrid->createSpectralArray( variable->getIndexInParentGrid() ) );

    //Compute FFT phase map of input
    spectral::array inputFFTimagPhase = getInputPhaseMap();

    //Compute varmap of input
    spectral::array inputVarmap = computeVarmap();

    //Initialize the optimization domain (boundary conditions) and
    //the sets of variogram paramaters (both linear and structured)
    VariogramParametersDomain domain;
    spectral::array vw;
    spectral::array L_wMin;
    spectral::array L_wMax;
    std::vector< IJVariographicStructure2D > variogramStructures;
    initDomainAndParameters( inputVarmap,
                             m,
                             domain,
                             vw,
                             L_wMin,
                             L_wMax,
                             variogramStructures );

    //-------------------------------------------------------------------------------------------------------------
    //------------------------------------- THE PARTICLE SWARM OPTIMIZATION ALGORITHM -----------------------------
    //-------------------------------------------------------------------------------------------------------------

    QProgressDialog progressDialog;
    progressDialog.setRange(0,0);
    progressDialog.show();
    progressDialog.setLabelText("Init particles...");
    QApplication::processEvents(); //let Qt update UI

    //Init the population of particles, their velocity vectors and their best position
    std::vector< spectral::array > particles_pw;
    std::vector< spectral::array > velocities_vw;
    std::vector< spectral::array > pbests_pbw;
    std::vector< double > fOfpbests;
    for( int iParticle = 0; iParticle < nParticles; ++iParticle ){
        //create a particle (one array of parameters)
        spectral::array pw( (spectral::index)( m * IJVariographicStructure2D::getNumberOfParameters() ) );
        //create a velocity vector (one array of velocities)
        spectral::array vw( pw.size() );
        //randomize the particle's position in the domain.
        for( int i = 0; i < pw.size(); ++i ){
            double LO = L_wMin[i];
            double HI = L_wMax[i];
            pw[i] = LO + std::rand() / (RAND_MAX/(HI-LO));
        }
        particles_pw.push_back( pw );
        //the velocities are initialized with zeros
        velocities_vw.push_back( vw );
        //the best position of a particle is initialized as the starting position
        spectral::array pbw( pw );
        pbests_pbw.push_back( pbw );
        //initialize the objective function value of the particle best as +inifinite
        fOfpbests.push_back( std::numeric_limits<double>::max() );
    }

    progressDialog.setLabelText("Get first global best position...");
    QApplication::processEvents(); //let Qt update UI

    //Init the global best postion (best of the best positions amongst the particles)
    spectral::array gbest_pw;
    double fOfgbest = std::numeric_limits<double>::max();
    {
        double fOfBest = std::numeric_limits<double>::max();
        for( int iParticle = 0; iParticle < nParticles; ++iParticle ){
            //get the best postition of a particle
            spectral::array& pbw = pbests_pbw[ iParticle ] ;
            //evaluate the objective function with the best position of a particle
            double f = objectiveFunction( *inputGrid, *inputData, pbw, m );
            //if it improves the value so far...
            if( f < fOfBest ){
                //...updates the best value record
                fOfBest = f;
                //...assigns the best of a particle as the global best
                gbest_pw = pbw;
            }
        }
    }

    progressDialog.setLabelText("Particle Swarm Optimization in progress...");
    progressDialog.setRange(0, maxNumberOfOptimizationSteps * nParticles );
    progressDialog.setValue( 0 );
    QApplication::processEvents(); //let Qt update UI

    //optimization steps
    for( int iStep = 0; iStep < maxNumberOfOptimizationSteps; ++iStep){

        //let Qt repaint the GUI every opt. step.
        QApplication::processEvents();

        //for each particle (vector of parameters)
        for( int iParticle = 0; iParticle < nParticles; ++iParticle ){

            //get the particle, its velocity and its best postion so far
            spectral::array& pw = particles_pw[ iParticle ];
            spectral::array& vw = velocities_vw[ iParticle ];
            spectral::array& pbw = pbests_pbw[ iParticle ];

            //get a candidate position and velocity of a particle
            spectral::array candidate_particle( pw.size() );
            spectral::array candidate_velocity( pw.size() );

            double rand1 = (std::rand()/(double)RAND_MAX);
            double rand2 = (std::rand()/(double)RAND_MAX);

            for( int i = 0; i < pw.size(); ++i ){
                candidate_velocity[i] = intertia_weight * vw[i] +
                                        acceleration_constant_1 * rand1 * ( pbw[i] - pw[i] ) +
                                        acceleration_constant_2 * rand2 * ( gbest_pw[i] - pw[i] );
                candidate_particle[i] = pw[i] + candidate_velocity[i];

                //performs a "bounce" of the particle if it "hits" the boundaries of the domain
                double overshoot = candidate_particle[i] - L_wMax[i];
                if( overshoot > 0 )
                    candidate_particle[i] = L_wMax[i] - overshoot;
                double undershoot = L_wMin[i] - candidate_particle[i];
                if( undershoot > 0 )
                    candidate_particle[i] = L_wMin[i] + undershoot;
            }

            //evaluate the objective function for current and candidate positions
            double fCurrent = objectiveFunction( *inputGrid, *inputData, pw, m );
            double fCandidate = objectiveFunction( *inputGrid, *inputData, candidate_particle, m );

            //if the candidate position improves the objective function
            if( fCandidate < fCurrent ){
                //update the postion
                pw = candidate_particle;
                //update the velocity
                vw = candidate_velocity;
            }

            //if the candidate position improves over the best of the particle
            if( fCandidate < fOfpbests[iParticle] ){
                //keep track of the best value of the objective function so far for the particle
                fOfpbests[iParticle] = fCandidate;
                //update the best position so far for the particle
                pbw = candidate_particle;
            }

            //if the candidate position improves over the global best
            if( fCandidate < fOfgbest ){
                //keep track of the global best value of the objective function
                fOfgbest = fCandidate;
                //update the global best position
                gbest_pw = candidate_particle;
            }

            //update progress bar
            progressDialog.setValue( iStep * nParticles + iParticle );
            QApplication::processEvents(); //let Qt update UI

        } // for each particle

        //collect the interation's objective function value
        s_objectiveFunctionValues.push_back( fOfgbest );

    } // for each step

    //-------------------------------------------------------------------------------------------------------------
    //-------------------------------------------------------------------------------------------------------------
    //-------------------------------------------------------------------------------------------------------------
    progressDialog.hide();

    //Read the optimized variogram model parameters back to the variographic structures
    for( int iParLinear = 0, iStructure = 0; iStructure < m; ++iStructure )
        for( int iPar = 0; iPar < IJVariographicStructure2D::getNumberOfParameters(); ++iPar, ++iParLinear )
            variogramStructures[iStructure].setParameter( iPar, gbest_pw[iParLinear] );

    // Display the results in a window.
    if( openResultsDialog ){
        displayResults( variogramStructures, inputFFTimagPhase, inputVarmap, false );
        showObjectiveFunctionEvolution();
    }

    //return the fitted model
    return variogramStructures;
}

std::vector<IJVariographicStructure2D> AutomaticVariogramFitting::processWithGenetic(
        int nThreads,
        int m,
        unsigned seed,
        int maxNumberOfGenerations,
        uint nPopulationSize,
        uint nSelectionSize,
        double probabilityOfCrossOver,
        uint pointOfCrossover,
        double mutationRate,
        bool openResultsDialog) const
{
    //clear the collected objective function values.
    s_objectiveFunctionValues.clear();

    //Intialize the random number generator with the same seed
    std::srand (seed);

    //the total number of genes (parameters) per individual.
    uint totalNumberOfParameters = m * IJVariographicStructure2D::getNumberOfParameters();

    //sanity checks
    if( nSelectionSize >= nPopulationSize ){
        QMessageBox::critical( Application::instance()->getMainWindow(),
                               "Error",
                               "AutomaticVariogramFitting::onDoWithGenetic(): Selection pool size must be less than population size.");
        return std::vector<IJVariographicStructure2D>();
    }
    if( nPopulationSize % 2 + nSelectionSize % 2 ){
        QMessageBox::critical( Application::instance()->getMainWindow(),
                               "Error",
                               "AutomaticVariogramFitting::onDoWithGenetic(): Sizes must be even numbers.");
        return std::vector<IJVariographicStructure2D>();
    }
    if( pointOfCrossover >= totalNumberOfParameters  ){
        QMessageBox::critical( Application::instance()->getMainWindow(),
                               "Error",
                               "AutomaticVariogramFitting::onDoWithGenetic(): Point of crossover must be less than the number of parameters.");
        return std::vector<IJVariographicStructure2D>();
    }

    // Get the data objects.
    IJAbstractCartesianGrid* inputGrid = m_cg;
    IJAbstractVariable* variable = m_at;

    // Get the grid's dimensions.
    unsigned int nI = inputGrid->getNI();
    unsigned int nJ = inputGrid->getNJ();
    unsigned int nK = inputGrid->getNK();

    // Fetch data from the data source.
    inputGrid->dataWillBeRequested();

    // Get the input data as a spectral::array object
    spectral::arrayPtr inputData( inputGrid->createSpectralArray( variable->getIndexInParentGrid() ) );

    //Compute FFT phase map of input
    spectral::array inputFFTimagPhase = getInputPhaseMap();

    //Compute varmap of input
    spectral::array inputVarmap = computeVarmap();

    //Initialize the optimization domain (boundary conditions) and
    //the sets of variogram paramaters (both linear and structured)
    VariogramParametersDomain domain;
    spectral::array vw;
    spectral::array L_wMin;
    spectral::array L_wMax;
    std::vector< IJVariographicStructure2D > variogramStructures;
    initDomainAndParameters( inputVarmap,
                             m,
                             domain,
                             vw,
                             L_wMin,
                             L_wMax,
                             variogramStructures );

    //=========================================THE GENETIC ALGORITHM==================================================

    //distribute as evenly as possible (load balance) the starting
    //points (by their indexes) amongst the threads.
    std::vector< std::pair< int, int > > individualsIndexesRanges =
            Util::generateSubRanges( 0, nPopulationSize-1, nThreads );

    //sanity check
    assert( individualsIndexesRanges.size() == nThreads && "AutomaticVariogramFitting::onDoWithGenetic(): "
                                                              "number of threads different from individual index ranges. "
                                                              " This is likely a bug in Util::generateSubRanges() function." );

    QProgressDialog progressDialog;
    progressDialog.setRange(0, maxNumberOfGenerations);
    progressDialog.setValue( 0 );
    progressDialog.show();
    progressDialog.setLabelText("Genetic Algorithm in progress...");

    //the main algorithm loop
    std::vector< Individual > population;
    for( int iGen = 0; iGen < maxNumberOfGenerations; ++iGen ){

        //Init or refill the population with randomly generated individuals.
        while( population.size() < nPopulationSize ){
            //create an individual (one array of parameters)
            spectral::array pw( (spectral::index)totalNumberOfParameters );
            //randomize the individual's position in the domain.
            for( int i = 0; i < pw.size(); ++i ){
                double LO = L_wMin[i];
                double HI = L_wMax[i];
                pw[i] = LO + std::rand() / (RAND_MAX/(HI-LO));
            }
            Individual ind( pw );
            population.push_back( ind );
        }

        //create and start the threads.  Each thread evaluates the objective function for a series of individuals.
        std::thread threads[nThreads];
        unsigned int iThread = 0;
        for( const std::pair< int, int >& individualsIndexesRange : individualsIndexesRanges ) {
            threads[iThread] = std::thread( taskEvaluateObjetiveInRangeOfIndividualsForGenetic,
                                            std::cref(*this),
                                            individualsIndexesRange.first,
                                            individualsIndexesRange.second,
                                            m,
                                            std::cref(*inputGrid),
                                            std::cref(*inputData), //<-- INPUT PARAMETERS UP TO HERE
                                            std::ref( population ) //--> OUTPUT PARAMETERS UP TO HERE
                                            );
            ++iThread;
        } //for each thread (ranges of starting points)

        //wait for the threads to finish.
        for( unsigned int iThread = 0; iThread < nThreads; ++iThread)
            threads[iThread].join();

        //sort the population in ascending order (lower value == better fitness)
        std::sort( population.begin(), population.end() );

        //collect the iteration's best objective function value
        s_objectiveFunctionValues.push_back( population[0].fValue );

        //clip the population (the excessive worst fit individuals die)
        while( population.size() > nPopulationSize )
            population.pop_back();

        //perform selection by binary tournament
        std::vector< Individual > selection;
        for( uint iSel = 0; iSel < nSelectionSize; ++iSel ){
            //perform binary tournament
            std::vector< Individual > tournament;
            {
                //draw two different individuals at random from the population for the tournament.
                int tournCandidate1 = std::rand() / (double)RAND_MAX * ( population.size() - 1 );
                int tournCandidate2 = tournCandidate1;
                while( tournCandidate2 == tournCandidate1 )
                    tournCandidate2 = std::rand() / (double)RAND_MAX * ( population.size() - 1 );
                //add the participants in the tournament
                tournament.push_back( population[tournCandidate1] );
                tournament.push_back( population[tournCandidate2] );
                //sort the binary tournament
                std::sort( tournament.begin(), tournament.end());
            }
            //add the best of tournament to the selection pool
            selection.push_back( tournament.front() );
        }

        //perform crossover and mutation on the selected individuals
        std::vector< Individual > nextGen;
        while( ! selection.empty() ){
            //draw two different selected individuals at random for crossover.
            int parentIndex1 = std::rand() / (double)RAND_MAX * ( selection.size() - 1 );
            int parentIndex2 = parentIndex1;
            while( parentIndex2 == parentIndex1 )
                parentIndex2 = std::rand() / (double)RAND_MAX * ( selection.size() - 1 );
            Individual parent1 = selection[ parentIndex1 ];
            Individual parent2 = selection[ parentIndex2 ];
            selection.erase( selection.begin() + parentIndex1 );
            selection.erase( selection.begin() + parentIndex2 );
            //draw a value between 0.0 and 1.0 from an uniform distribution
            double p = std::rand() / (double)RAND_MAX;
            //if crossover is due...
            if( p < probabilityOfCrossOver ){
                //crossover
                std::pair< Individual, Individual> offspring = parent1.crossOver( parent2, pointOfCrossover );
                Individual child1 = offspring.first;
                Individual child2 = offspring.second;
                //mutate all
                child1.mutate( mutationRate, L_wMin, L_wMax );
                child2.mutate( mutationRate, L_wMin, L_wMax );
                parent1.mutate( mutationRate, L_wMin, L_wMax );
                parent2.mutate( mutationRate, L_wMin, L_wMax );
                //add them to the next generation pool
                nextGen.push_back( child1 );
                nextGen.push_back( child2 );
                nextGen.push_back( parent1 );
                nextGen.push_back( parent2 );
            } else { //no crossover took place
                //simply mutate and insert the parents into the next generation pool
                parent1.mutate( mutationRate, L_wMin, L_wMax );
                parent2.mutate( mutationRate, L_wMin, L_wMax );
                nextGen.push_back( parent1 );
                nextGen.push_back( parent2 );
            }
        }

        //make the next generation
        population = nextGen;

        //update progress bar
        progressDialog.setValue( iGen );
        QApplication::processEvents(); // let Qt update the UI

    } //main algorithm loop

    //=====================================GET RESULTS========================================
    progressDialog.hide();

    //evaluate the individuals of final population
    for( uint iInd = 0; iInd < population.size(); ++iInd ){
        Individual& ind = population[iInd];
        ind.fValue = objectiveFunction( *inputGrid, *inputData, ind.parameters, m );
    }

    //sort the population in ascending order (lower value == better fitness)
    std::sort( population.begin(), population.end() );

    //get the parameters of the best individual (set of parameters)
    spectral::array gbest_pw = population[0].parameters;

    //Read the optimized variogram model parameters back to the variographic structures
    for( int iParLinear = 0, iStructure = 0; iStructure < m; ++iStructure )
        for( int iPar = 0; iPar < IJVariographicStructure2D::getNumberOfParameters(); ++iPar, ++iParLinear )
            variogramStructures[iStructure].setParameter( iPar, gbest_pw[iParLinear] );

    // Display the results in a window.
    if( openResultsDialog ){
        displayResults( variogramStructures, inputFFTimagPhase, inputVarmap, false );
        showObjectiveFunctionEvolution();
    }

    //return the fitted model
    return variogramStructures;
}

double AutomaticVariogramFitting::evaluateModel(const std::vector<IJVariographicStructure2D> &variogramStructures) const
{
    // Get the number of nested structures
    int m = variogramStructures.size();
    // Get the input grid data
    spectral::arrayPtr inputData( m_cg->createSpectralArray( m_at->getAttributeGEOEASgivenIndex()-1 ) ) ;
    //Read the optimized variogram model parameters as a linearized array
    spectral::array vw( variogramStructures.size() * IJVariographicStructure2D::getNumberOfParameters() );
    for( int i = 0, iStructure = 0; iStructure < m; ++iStructure )
        for( int iPar = 0; iPar < IJVariographicStructure2D::getNumberOfParameters(); ++iPar, ++i )
            vw[i] = variogramStructures[iStructure].getParameter( iPar );

    return objectiveFunction( *m_cg, *inputData, vw, m );
}

void AutomaticVariogramFitting::printModel(const std::vector<IJVariographicStructure2D> &model,
                                           bool breakLineAtEachStructure)
{
    for( const IJVariographicStructure2D& structure : model ){
        std::cout << structure.range << '\t'
                  << structure.rangeRatio << '\t'
                  << structure.azimuth << '\t'
                  << structure.contribution << '\t';
        if( breakLineAtEachStructure )
            std::cout << std::endl;
    }
    if( ! breakLineAtEachStructure )
        std::cout << std::endl;
}

void AutomaticVariogramFitting::movePointAlongLineForLSRS(int m,
        int i,
        int k,
        const VariogramParametersDomain &domain,
        const spectral::array& L_wMax,
        const spectral::array& L_wMin,
        const IJAbstractCartesianGrid& inputGrid,
        const spectral::array& inputData,
        const spectral::array& randSequence,
        std::vector<spectral::array> &startingPoints, //--> Output parameter
        double &fOfBestSolution,                      //--> Output parameter
        spectral::array &vw_bestSolution              //--> Output parameter
        ) const
{
    std::unique_lock<std::mutex> LSRSlock ( myMutexLSRS, std::defer_lock );

    //lambda to define the step as a function of iteration number (the alpha-k in Grosan and Abraham (2009))
    //first iteration must be 1.
    auto alpha_k = [=](int k) { return 2.0 + 3.0 / std::pow(2, k*k + 1); };

    double deltaAxis         = domain.max.range        - domain.min.range;
    double deltaRatio        = domain.max.rangeRatio   - domain.min.rangeRatio;
    double deltaAzimuth      = domain.max.azimuth      - domain.min.azimuth;
    double deltaContribution = domain.max.contribution - domain.min.contribution;

    //make a candidate point with a vector from the current point.
    spectral::array vw_candidate( (spectral::index)( m * IJVariographicStructure2D::getNumberOfParameters() ) );
    for( int j = 0; j < vw_candidate.size(); ++j ){
        double p_k = -1.0 + randSequence( j, i, k-1 ) * 2.0;//author suggests -1 or drawn from [0.0 1.0] for best results

        double delta = 0.0;
        switch( j % IJVariographicStructure2D::getNumberOfParameters() ){
        case 0: delta = deltaAxis;         break;
        case 1: delta = deltaRatio;        break;
        case 2: delta = deltaAzimuth;      break;
        case 3: delta = deltaContribution; break;
        }

        vw_candidate[j] = startingPoints[i][j] + p_k * delta * alpha_k( k );
        if( vw_candidate[j] > L_wMax[j] )
            vw_candidate[j] = L_wMax[j];
        if( vw_candidate[j] < L_wMin[j] )
            vw_candidate[j] = L_wMin[j];

    }
    //evaluate the objective function for the current point and for the candidate point
    double fCurrent   = objectiveFunction( inputGrid, inputData, startingPoints[i], m );
    double fCandidate = objectiveFunction( inputGrid, inputData, vw_candidate,      m );
    //if the candidate point improves the objective function...
    if( fCandidate < fCurrent ){
        LSRSlock.lock();   //----------------------> Data writing section protected with a mutex lock
        //...make it the current point.
        startingPoints[i] = vw_candidate;
        //keep track of the best solution
        if( fCandidate < fOfBestSolution ){
            fOfBestSolution = fCandidate;
            vw_bestSolution = vw_candidate;
        }
        LSRSlock.unlock(); //<---------------------- Data writing section protected with a mutex lock
    }
}

