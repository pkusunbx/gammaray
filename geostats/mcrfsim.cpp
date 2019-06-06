#include "mcrfsim.h"

#include "gslib/gslibparameterfiles/commonsimulationparameters.h"
#include "domain/attribute.h"
#include "domain/cartesiangrid.h"
#include "domain/categorypdf.h"
#include "domain/verticaltransiogrammodel.h"
#include "domain/categorydefinition.h"
#include "domain/application.h"
#include "geostats/searchneighborhood.h"
#include "geostats/searchellipsoid.h"

#include <thread>
#include <random>
#include <QApplication>
#include <QProgressDialog>

MCRFSim::MCRFSim() :
    m_atPrimary( nullptr),
    m_cgSim( nullptr ),
    m_pdf( nullptr ),
    m_transiogramModel( nullptr ),
    m_lateralGradationType( LateralGradationType::TAIL_TRANSIOGRAMS_ONLY ),
    m_gradationField( nullptr ),
    m_LVAazimuth( nullptr ),
    m_LVAsemiMajorAxis( nullptr ),
    m_LVAsemiMinorAxis( nullptr ),
    m_probFields( std::vector< Attribute*>() ),
    m_tauFactorForGlobalPDF( 1.0 ),
    m_tauFactorForTransiography( 1.0 ),
    m_tauFactorForProbabilityFields( 1.0 ),
    m_commonSimulationParameters( nullptr ),
    m_progressDialog( nullptr )
{ }

bool MCRFSim::isOKtoRun()
{
    if( ! m_atPrimary ){
        m_lastError = "Categorical variable not provided.";
        return false;
    }

    if( ! m_cgSim ){
        m_lastError = "Simulation grid not provided.";
        return false;
    } else if( ! m_cgSim->hasNoDataValue() ) {
        m_lastError = "Simulation grid does not have a No-Data value configured.";
        return false;
    }

    if( ! m_pdf ){
        m_lastError = "Global PDF not provided.";
        return false;
    } else {
        DataFile* dfPrimary = dynamic_cast<DataFile*>( m_atPrimary->getContainingFile() );
        if( ! dfPrimary ){
            m_lastError = "The file of input categorical variable is not a DataFile object.";
            return false;
        }
        CategoryDefinition* cdOfPrimData = dfPrimary->getCategoryDefinition( m_atPrimary );
        if( ! cdOfPrimData ){
            m_lastError = "Category definition of input variable not found (nullptr).";
            return false;
        }
        CategoryDefinition* cdOfPDF = m_pdf->getCategoryDefinition();
        if( ! cdOfPDF ){
            m_lastError = "Category definition of PDF not found (nullptr).";
            return false;
        }
        if( cdOfPDF != cdOfPrimData ){
            m_lastError = "Category definition of input variable must be the same object as that the PDF is based on.";
            return false;
        }
    }

    if( ! m_transiogramModel ){
        m_lastError = "Vertical transiogram model not provided.";
        return false;
    } else {
        DataFile* dfPrimary = dynamic_cast<DataFile*>( m_atPrimary->getContainingFile() );
        CategoryDefinition* cdOfPrimData = dfPrimary->getCategoryDefinition( m_atPrimary );
        CategoryDefinition* cdOfTransiogramModel = m_transiogramModel->getCategoryDefinition();
        if( ! cdOfTransiogramModel ){
            m_lastError = "Category definition of vertical transiogram model not found (nullptr).";
            return false;
        }
        if( cdOfTransiogramModel != cdOfPrimData ){
            m_lastError = "Category definition of input variable must be the same object as that the vertical transiogram model is based on.";
            return false;
        }
    }

    if( m_lateralGradationType == LateralGradationType::USE_GRADATIONAL_FIELD && ! m_gradationField ){
        m_lastError = "Use of a gradation field was selected for lateral transiography ranging, but none was provided.";
        return false;
    }

    if( ( m_lateralGradationType == LateralGradationType::TAIL_TRANSIOGRAMS_ONLY ||
          m_lateralGradationType == LateralGradationType::HEAD_TRANSIOGRAMS_ONLY ||
          m_lateralGradationType == LateralGradationType::HEAD_AND_TAIL_TRANSIOGRAMS_AT_RANDOM )
           && ( ! m_LVAazimuth || ! m_LVAsemiMajorAxis || ! m_LVAsemiMinorAxis ) ){
        m_lastError = "Use of lateral transiogram ranges was selected, which requires three additional fields"
                      " in the simulation grid: azimuth, semi-major axis and semi-minor axis.";
        return false;
    }

    if( useSecondaryData() ){
        DataFile* dfPrimary = dynamic_cast<DataFile*>( m_atPrimary->getContainingFile() );
        CategoryDefinition* cdOfPrimData = dfPrimary->getCategoryDefinition( m_atPrimary );
        int nProbFields = m_probFields.size();
        int nCategories = cdOfPrimData->getCategoryCount();
        if( nProbFields != nCategories ){
            m_lastError = " Number of probability fields ( " +
                    QString::number(nProbFields) + " ) differs from the number of categories ( " +
                    QString::number(nCategories) + " ).";
            return false;
        }
    }

    if( ! m_commonSimulationParameters ){
        m_lastError = "A common simulation parameter object was not provided.  This object contains non-Markov-specific parameters such"
                      " as neighborhood parameters, random number generator seed, number of realization, etc.";
        return false;
    } else {
        if( m_commonSimulationParameters->getNumberOfRealizations() > 99 || m_commonSimulationParameters->getNumberOfRealizations() < 1 ){
            m_lastError = "Number of realizations must be between 1 and 99.";
            return false;
        }
    }

    return true;
}

bool MCRFSim::useSecondaryData()
{
    return ! m_probFields.empty();
}

double MCRFSim::simulateOneCell( uint i, uint k, uint l ) const
{
    return m_simGridNDV;
}

/** ///////////// Simulate some realizations in a separate thread. /////////////////////////
 * @param nRealsForOneThread The number of realizations the thread should simulate.
 * @param cgSim The simulation grid.
 * @param seed The seed for the random number generator (should be different from those of the other threads)
 * @param mcrfSim The pointer to the MCRFSim object coordinating the simulation.
 * @param completedFlag The pointer to an output boolean variable.
 *                      It'll receive the "true" value upon completion of all simulations.
 * @param realizations A pointer to a vector of spectral::array objects where the thread will deposit simulated data.
 *                     Each spectral::array contains the simulated data of one realization.
 *//////////////////////////////////////////////////////////////////////////////////////////
void simulateSomeRealizationsThread( uint nRealsForOneThread,
                                     const CartesianGrid* cgSim,
                                     uint seed,
                                     MCRFSim* mcrfSim,
                                     bool* completedFlag,
                                     std::vector< spectral::arrayPtr >* realizationsOutput ){

    //initialize the thread-local random number generator with the seed reserved for this thread
    std::mt19937 randomNumberGenerator;
    randomNumberGenerator.seed( seed );

    //define a uniform distribution
    std::uniform_int_distribution<long> distribution( 0, RAND_MAX );

    //get simulation grid dimensions
    uint nI = cgSim->getNI();
    uint nJ = cgSim->getNJ();
    uint nK = cgSim->getNK();
    ulong nCells = nI * nJ * nK;

    ulong numberOfSimulationsExecuted = 0;
    ulong reportProgressEveryNumberOfSimulations = 1000;

    // A lambda function for the random walk generation
    // Note: the "mutable" keyword is in the lambda declaration because we need to capture the distribution and random
    // number generator objects as non-const references, as inherently using them changes their state.
    auto lambdaFnShuffler = [ distribution, randomNumberGenerator ] (int i) mutable {
        return static_cast<int>( distribution(randomNumberGenerator) % i );
    };

    //for each realization of this thread
    for( uint iRealization = 0; iRealization < nRealsForOneThread; ++iRealization ){

        //init realization data with the sim grid's NDV
        spectral::arrayPtr simulatedData = spectral::arrayPtr( new spectral::array( nI, nJ, nK, cgSim->getNoDataValueAsDouble() ) );

        //prepare a vector with the random walk (sequence of linear cell indexes to simulate)
        std::vector<ulong> linearIndexesRandomWalk;
        linearIndexesRandomWalk.reserve( nCells );
        for (int i=0; i<nCells; ++i) linearIndexesRandomWalk.push_back(i);

        // shuffles the cell linear indexes to make the random walk.
        std::random_shuffle( linearIndexesRandomWalk.begin(), linearIndexesRandomWalk.end(), lambdaFnShuffler );

        //traverse the grid's cells according to the random walk.
        for( uint iRandomWalkIndex = 0; iRandomWalkIndex < nCells; ++iRandomWalkIndex ){
            //get the cell's linear index
            uint iCellLinearIndex = linearIndexesRandomWalk[ iRandomWalkIndex ];
            //get the IJK cell index
            uint i, j, k;
            cgSim->indexToIJK( iCellLinearIndex, i, j, k );
            //simulate the cell (attention: may return the simulation grid's no-data value)
            double catCode = mcrfSim->simulateOneCell( i, j, k );
            //save the value to the data array of the realization
            (*simulatedData)( i, j, k ) = catCode;
            //keep track of simulation progress
            ++numberOfSimulationsExecuted;
            if( ! ( numberOfSimulationsExecuted % reportProgressEveryNumberOfSimulations ) )
                mcrfSim->setOrIncreaseProgress( reportProgressEveryNumberOfSimulations );
        } //grid traversal (random walk)

        //return the realization data
        realizationsOutput->push_back( simulatedData );

    } // for each reazation of this thread

    //signals the client code that this thread finished
    *completedFlag = true;
}
///////////////////////////////////////////////////////////////////////////////


bool MCRFSim::run()
{
    //check whether everything is ok
    if( !isOKtoRun() )
        return false;

    //sets progress count to zero
    m_progress = 0;

    //clears any previous realization data
    m_realizations.clear();

    //get simulation grid dimensions
    uint nI = m_cgSim->getNI();
    uint nJ = m_cgSim->getNJ();
    uint nK = m_cgSim->getNK();

    //get the simulation grid's NDV (getNoDataValueAsDouble() is expensive)
    m_simGridNDV = m_cgSim->getNoDataValueAsDouble();

    //get the number of realizations the user wants to simulate
    uint nRealizations = m_commonSimulationParameters->getNumberOfRealizations();

    //get the number of threads from logical CPUs or number of realizations (whichever is the lowest)
    unsigned int nThreads = std::min( std::thread::hardware_concurrency(), nRealizations );

    //announce the simulation has begun.
    Application::instance()->logInfo("Commencing MCRF simulation with " + QString::number(nThreads) + " thread(s).");

    //distribute the realizations among the n-threads
    uint numberOfRealizationsForAThread[nThreads];
    for( uint iThread = 0; iThread < nThreads; ++iThread )
        numberOfRealizationsForAThread[ iThread ] = 0; //initialize the numbers of realizations with zeros
    for( uint iReal = 0; iReal < nRealizations; ++iReal )
        ++numberOfRealizationsForAThread[ iReal % nThreads ];

    //create vectors of data arrays for each thread, so they deposit their realizations in them.
    std::vector< spectral::arrayPtr > realizationDepots[nThreads];

    //configure and display a progress bar
    //////////////////////////////////
    m_progressDialog = new QProgressDialog;
    m_progressDialog->show();
    m_progressDialog->setLabelText("Running MCRF...");
    m_progressDialog->setMinimum(0);
    m_progressDialog->setValue(0);
    m_progressDialog->setMaximum( nI * nJ * nK * nRealizations );
    /////////////////////////////////

    //create an array of flags that tells whether a thread is completed.
    // intialize all with false
    bool completed[ nThreads ];
    for( uint iThread = 0; iThread < nThreads; ++iThread )
        completed[ iThread ] = false;

    // Build the search strategy.
    {
        double hMax              =         m_commonSimulationParameters->getSearchEllipHMax();
        double hMin              =         m_commonSimulationParameters->getSearchEllipHMin();
        double hVert             =         m_commonSimulationParameters->getSearchEllipHVert();
        double azimuth           =         m_commonSimulationParameters->getSearchEllipAzimuth();
        double dip               =         m_commonSimulationParameters->getSearchEllipDip();
        double roll              =         m_commonSimulationParameters->getSearchEllipRoll();
        uint nb_samples          =         m_commonSimulationParameters->getNumberOfSamples();
        uint min_nb_samples      =         m_commonSimulationParameters->getMinNumberOfSamples();
        uint numberOfSectors     =         m_commonSimulationParameters->getNumberOfSectors();
        uint minSamplesPerSector =         m_commonSimulationParameters->getMinNumberOfSamplesPerSector();
        uint maxSamplesPerSector =         m_commonSimulationParameters->getMaxNumberOfSamplesPerSector();
        double minDistanceBetweensamples = m_commonSimulationParameters->getMinDistanceBetweenSecondaryDataSamples();
        SearchNeighborhoodPtr searchNeighborhood(
                    new SearchEllipsoid(hMax, hMin, hVert,
                                        azimuth, dip, roll,
                                        numberOfSectors, minSamplesPerSector, maxSamplesPerSector
                                        )
                    );
        m_searchStrategy = SearchStrategyPtr( new SearchStrategy( searchNeighborhood, nb_samples, minDistanceBetweensamples, min_nb_samples ) );
    }

    //create and run the simulation threads
    std::thread threads[nThreads];
    for( unsigned int iThread = 0; iThread < nThreads; ++iThread){
        std::vector< spectral::arrayPtr >& realizationDepot = realizationDepots[iThread];
        //Give a different seed to each thread by multiplying the user-given seed by 100 and adding the thread number
        //NOTE on the seed number * 100:
        //number of realizations is capped at 99, so even if there are more than 99
        //logical processors, number of threads will be limited to 99
        threads[iThread] = std::thread( simulateSomeRealizationsThread,
                                        numberOfRealizationsForAThread[ iThread ],
                                        m_cgSim,
                                        m_commonSimulationParameters->getSeed() * 100 + iThread,
                                        this,
                                        &(completed[ iThread ]),
                                        &realizationDepot
                                        );
    }

    //this non-locking loop allows the progress dialog to update (Qt runs in this thread).
    //while the worker threads run.
    bool allThreadsFinished = false;
    int progressRefreshCount = 0;
    int progressRefreshRate = 1000000; // the greater the number, less frequent Qt repaints
    while( ! allThreadsFinished ){
        //monitor the finished flags for all running threads
        allThreadsFinished = true;
        for( unsigned int iThread = 0; iThread < nThreads; ++iThread)
            if( ! completed[iThread] )
                allThreadsFinished = false;
        //allows Qt to redraw stuff as well as respond to events from time to time.
        ++progressRefreshCount;
        if( progressRefreshCount > progressRefreshRate ){
            updateProgessUI();
            progressRefreshCount = 0;
        }
    }

    //lock-wait for the threads to finish their execution contexts.
    for( unsigned int iThread = 0; iThread < nThreads; ++iThread)
        threads[iThread].join();

    //collect the realizations created by the threads:
    for( unsigned int iThread = 0; iThread < nThreads; ++iThread ){
        std::vector< spectral::arrayPtr >::iterator it = realizationDepots[ iThread ].begin();
        for(; it != realizationDepots[ iThread ].end(); ++it)
            m_realizations.push_back( *it );
    }

    //hide the progress dialog
    delete m_progressDialog;

    //announce the simulation has completed with success
    Application::instance()->logInfo("MCRF completed.");
    return true;
}

void MCRFSim::setOrIncreaseProgress(ulong ammount, bool increase)
{
    std::unique_lock<std::mutex> lck ( m_mutexMCRF, std::defer_lock );
    lck.lock(); //this code is expected to be called concurrently from multiple simulation threads
    if( increase )
        m_progress += ammount;
    else
        m_progress = ammount;
    lck.unlock();
}

void MCRFSim::updateProgessUI()
{
    m_progressDialog->setValue( m_progress );
    QApplication::processEvents();
}
