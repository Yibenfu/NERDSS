/* \file vector_rd_reweight_NOPBC.cpp
 * \brief Main function for simulation.
 *
 * ## TODO
 *  - Change freelist from unordered list of relative iface indices to ordered list of absolute indices.
 *    really speed up evaluate_binding_pair
 *  - Write species tracker (tracks connectivity)
 *  - Compress reflect_traj_complex_rad_rot, reflect_traj_check_span, reflect_traj_rad_rot_nocheck
 *  - IDEA: move copy numbers from .mol files to .inp file, like in BNGL.
 *
 */

#include "boundary_conditions/reflect_functions.hpp"
#include "io/io.hpp"
#include "math/constants.hpp"
#include "math/matrix.hpp"
#include "math/rand_gsl.hpp"
#include "parser/parser_functions.hpp"
#include "reactions/association/association.hpp"
#include "reactions/bimolecular/bimolecular_reactions.hpp"
#include "reactions/implicitlipid/implicitlipid_reactions.hpp"
#include "reactions/shared_reaction_functions.hpp"
#include "reactions/unimolecular/unimolecular_reactions.hpp"
#include "system_setup/system_setup.hpp"
#include "trajectory_functions/trajectory_functions.hpp"

#include <chrono>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

// easier to read timer
using MDTimer = std::chrono::system_clock;
using timeDuration = std::chrono::duration<double, std::chrono::seconds>;

/* INITIALIZE GLOBALS */
long long randNum = 0;
unsigned long totMatches = 0;

int main(int argc, char* argv[])
{
    /* SIMULATION SETUP */
    // Get seed for random number generation
    // use random_device instead of time so that multiple jobs started at the same time have more unique seeds
    std::random_device rd {};
    unsigned seed { rd() };
    //    seed = 305082827;

    // get time point for total simulation time
    MDTimer::time_point totalTimeStart = MDTimer::now();

    /* SET UP SIMULATION LISTS */
    // Simulation species lists
    std::vector<MolTemplate> molTemplateList {}; // list of provided molecule templates
    std::vector<std::vector<int>> molecTypesList {};

    // Reaction lists
    std::vector<ForwardRxn> forwardRxns {}; // list of forward reactions
    std::vector<BackRxn> backRxns {}; // list of back reactions (corresponding to forward reactions)
    std::vector<CreateDestructRxn> createDestructRxns {}; // list of creation and destruction reactions
    forwardRxns.reserve(10);
    backRxns.reserve(10);

    /* PARSE INPUT */
    Parameters params {};
    std::string paramFile {};

    // set up some output files
    // TODO: change these to open and close as needed
    // std::ofstream timeStatFile("time_molec_number.dat");
    //     std::ofstream timeStatFileText("time_molec_text.dat");
    //     std::ofstream molecTypesFile("molectypes.dat");
    //     std::ofstream complexFile("complex_components.dat");

    std::string observablesFileName { "species.dat" };
    std::string trajFileName { "trajectory.xyz" };
    std::string restartFileName { "restart.dat" };
    std::string restartFileNameInput;//if you read it in, allow it to have its own name.
    //std::string speciesFileName { "all_species.dat" };
    //    std::string speciesFileName { "all_species_rank" };//dat file
    //char rankChar[10];
    //sprintf(rankChar, "%d",rank);
    //speciesFileName+=rankChar;
    //speciesFileName.append(".dat");

    // TODO: Write command line flag parser and get this out of here
    unsigned verbosity { 0 };
    std::cout << "Command: " << std::string(argv[0]) << std::flush;
    for (int flagItr { 1 }; flagItr < argc; ++flagItr) {
        std::string flag { argv[flagItr] };
        std::cout << ' ' << flag << std::flush;
        if (flag == "-f") {
            paramFile = argv[flagItr + 1];
            std::cout << ' ' << std::string(argv[flagItr + 1]) << std::flush;
            ++flagItr;
        } else if (flag == "-s" || flag == "--seed") {
            std::stringstream iss(argv[flagItr + 1]);
            unsigned tmpseed;
            if (iss >> tmpseed)
                seed = tmpseed;
            else {
                std::cerr << "Error reading seed, exiting.\n";
                exit(1);
            }
            ++flagItr;
            std::cout << '\n';
        } else if (flag == "--debug-force-dissoc") {
            params.debugParams.forceDissoc = true;
        } else if (flag == "--debug-force-assoc") {
            params.debugParams.forceAssoc = true;
        } else if (flag == "--print-system-info") {
            params.debugParams.printSystemInfo = true;
        } else if (flag == "-r" || flag == "--restart") {
            restartFileNameInput = std::string(argv[flagItr + 1]);
            params.fromRestart = true;
            std::cout << ' ' << std::string(argv[flagItr + 1]) << std::flush;
            ++flagItr;
        } else if (flag == "-v") {
            params.debugParams.verbosity = 1;
        } else if (flag == "-vv") {
            verbosity = 2;
            // } else if (std::string{ argv[flag] } == "-o") {
            //     trajFile = std::ofstream( std::string{ argv[flag+1] } + std::string{".xyz"} );
            //     ++flag;
        } else {
            std::cout << " ignored " << std::endl;
        }
    }

    auto startTime = MDTimer::to_time_t(totalTimeStart);
    char charTime[24];
    //std::cout << "\nStart date: " << std::put_time(std::localtime(&startTime), "%F %T") << '\n';
    std::cout << "\nStart date: ";
    if(0 < strftime(charTime, sizeof(charTime), "%F %T", std::localtime(&startTime)) ) std::cout << charTime << '\n';
    std::cout << "Seed: " << seed << std::endl;
    srand_gsl(seed);

    /* SET UP SOME IMPORTANT VARIABLES */
    // 2D reaction probability tables
    std::vector<gsl_matrix*> survMatrices; // used in evaluate_binding_pair_com
    std::vector<gsl_matrix*> normMatrices; // idem
    std::vector<gsl_matrix*> pirMatrices; // idem
    double* tableIDs = new double[params.max2DRxns * 2]; // TODO: Change this?

    /* SET UP SYSTEM */
    //    std::vector<SpeciesTracker::Observable> observablesList;
    std::map<std::string, int> observablesList;
    SimulVolume simulVolume {};
    std::vector<Molecule> moleculeList {}; // list of all species in the system
    std::vector<Complex> complexList {};

    /* EMPTY MOLECULE LISTS FOR NONEQUILIBRIUM SIMULATIONS */
    std::vector<int> emptyComList {}; // list of elements in complexList which are empty
    std::vector<int> emptyMolList {}; // list of elements in moleculeList which are empty
    
    Membrane membraneObject;//class structure that contains boundary conditions, implicit lipid model. 

    // Initialize and write initial copy numbers for all species
    copyCounters counterArrays; // contains arrays tracking bound molecule pairs, and species copy nums.
    
    int implicitlipidIndex {0}; // implicit-lipid index, which is also stored in membraneObject.implicitlipidIndex.


    unsigned simItr { 0 };
    if (!params.fromRestart && paramFile != "") {
        // Parse the input files
        parse_input(paramFile, params, observablesList, forwardRxns, backRxns, createDestructRxns, molTemplateList, membraneObject);
        MolTemplate::numMolTypes = molTemplateList.size();
        
        std::cout << " NUMBER OF MOLECULE TYPES: " << params.numMolTypes
                  << " NUMBER OF INTERFACES PLUS STATES, including PRODUCTS: " << params.numTotalSpecies << std::endl;
        // set up prior Observables

        // write the Observables file header and initial values
        std::ofstream observablesFile { observablesFileName };
        if (observablesList.size() == 1) {
            observablesFile << "Itr," << observablesList.begin()->first << '\n';
            observablesFile << "0,0\n";
        } else if (observablesList.size() > 1) {
            observablesFile << "Itr";
            for (auto obsItr = observablesList.begin(); obsItr != observablesList.end(); ++obsItr)
                observablesFile << ',' << obsItr->first;
            std::cout << "\n0";
            for (auto obsItr = observablesList.begin(); obsItr != observablesList.end(); ++obsItr)
                std::cout << ',' << obsItr->second;
            observablesFile << '\n';
        }

        observablesFile.close();	

	

        unsigned long reservation {};
        for (auto& molTemp : molTemplateList)
            reservation += molTemp.copies;
        moleculeList.reserve(reservation);
        complexList.reserve(reservation);

        // generate the system coordinates, write out coordinate and topology files
        generate_coordinates(params, moleculeList, complexList, molTemplateList, forwardRxns, membraneObject);
        write_psf(params, moleculeList, molTemplateList);
        
        // set up some important parameters for implicit-lipid model;
        initialize_paramters_for_implicitlipid_model(implicitlipidIndex, params, forwardRxns, backRxns,
                                                     moleculeList, molTemplateList,complexList, membraneObject);
	membraneObject.No_free_lipids=membraneObject.nSites;
        /* CREATE SIMULATION BOX CELLS */
        std::cout << llinebreak << bon << "Partitioning simulation box into sub-boxes...\n" << boff;
        set_rMaxLimit(params, molTemplateList, forwardRxns);
        simulVolume.create_simulation_volume(params, membraneObject);
        simulVolume.update_memberMolLists(params, moleculeList, complexList, molTemplateList, membraneObject, simItr);
        simulVolume.display();

        // write beginning of trajectory
        std::ofstream trajFile { trajFileName };
        write_traj(0, trajFile, params, moleculeList, molTemplateList, membraneObject);
        trajFile.close();
        // write_restart.cpp
    } else if (params.fromRestart){// && paramFile.empty()) {
        // read from restart
        rand_gsl(); // use the RNG once to initialize, otherwise reading in RNG state won't work
        read_rng_state(); // read the current RNG state
        --randNum; // TODO: TEMPORARY
        std::ifstream restartFileInput { restartFileNameInput };
        if (!restartFileInput)
            std::cerr << "Warning, could not find restart file. Using new seed.\n";

        read_restart(simItr, restartFileInput, params, simulVolume, moleculeList, complexList, molTemplateList, forwardRxns,
                     backRxns, createDestructRxns, observablesList, emptyMolList, emptyComList, membraneObject);
        restartFileInput.close();
	double storeRS3D=membraneObject.RS3D;
        std::cout << " Total number of states (reactant and product)  in the system " << RxnBase::totRxnSpecies
                  << std::endl;
        params.numTotalSpecies = RxnBase::totRxnSpecies;
	std::cout <<" Total number of molecules: "<<Molecule::numberOfMolecules<<" Size of molecule list : "<<moleculeList.size()<<std::endl;
	std::cout <<"Total number of complexes: "<< Complex::numberOfComplexes<<" size of list: "<< complexList.size()<<std::endl;

	// set up some important parameters for implicit-lipid model;
        initialize_paramters_for_implicitlipid_model(implicitlipidIndex, params, forwardRxns, backRxns,
                                                     moleculeList, molTemplateList,complexList, membraneObject);

	membraneObject.RS3D=storeRS3D;
	std::cout <<" RS3D after restart: "<<membraneObject.RS3D<<std::endl;
	/* CREATE SIMULATION BOX CELLS */
        std::cout << llinebreak << bon << "Partitioning simulation box into sub-boxes...\n" << boff;
        set_rMaxLimit(params, molTemplateList, forwardRxns);
        simulVolume.create_simulation_volume(params, membraneObject);
        simulVolume.update_memberMolLists(params, moleculeList, complexList, molTemplateList, membraneObject, simItr);
        simulVolume.display();

	// Check to make sure the trajectory length matches the restart file

	std::cout<<" params.trajFile: "<<params.trajFile<<std::endl;
        std::ifstream trajFile { params.trajFile };
        int trajItr { -1 };
        if (trajFile) {
            std::string line;
            while (getline(trajFile, line)) {
                auto headerItr = line.find(':');
                if (headerItr != std::string::npos) {
                    trajItr = std::stoi(line.substr(headerItr + 1, std::string::npos)); // + 1 to ignore the colon
                }
            }
            if (trajItr == simItr) {
                std::cout << "Trajectory length matches provided restart file. Continuing...\n";
            } else {
                std::cerr << "ERROR: Trajectory length doesn't match provided restart file. Exiting...\n";
                exit(1);
            }
            trajFile.close();
        } else {
            std::cout << "WARNING: No trajectory found, writing new trajectory.\n";
        }

	
	

        //        std::ifstream obsFile { observablesFileName };
        //        if (obsFile) {
        //            std::string line;
        //            int obsItr { -1 };
        //            getline(obsFile, line); // skips the header
        //            while (getline(obsFile, line)) {
        //                auto headerItr = line.find(',');
        //                if (headerItr != std::string::npos) {
        //                    obsItr = std::stoi(line.substr(0, headerItr)); // + 1 to ignore the colon
        //                }
        //            }
        //            if (obsItr == simItr) {
        //                std::cout << "Observables file length matches provided restart file. Continuing...\n";
        //                std::cout << "Current observables:\n";
        //                for (auto& elem : observablesList)
        //                    std::cout << '\t' << elem.first << ' ' << elem.second << '\n';
        //            } else {
        //                std::cerr << "ERROR: Observables file length doesn't match provided restart file.
        //                Exiting...\n"; exit(1);
        //            }
        //            obsFile.close();
        //        } else {
        //            std::cout << "WARNING: No observables file found, writing new observables file.\n";
        //        }


    } else {
        std::cerr << "Please provide a parameter and/or restart file. Parameter file Syntax is : ./rd_executable.exe "
                     "-f parameterfile.inp \n";
        exit(1);
    }

    /*Print out parameters*/
    params.display();
    std::cout <<" BOX DIMENSIONS, X,Y,Z: "<<membraneObject.waterBox.x<<' '<<membraneObject.waterBox.y<<' '<<membraneObject.waterBox.z<<'\n';

    /*Determine whether the surface has adsorption properties:
      Do any molecules bind directly to surface using our continuum model?
      For each molecule .bindToSurface=0 if NO.
    */

    /* SETUP OUTPUT FILES */
    /*output files reporting bound pairs, and histogram of complex components*/

    // the variable params.numIfaces does not seem to be used anywhere for anything. It is also not correctly
    // initialized anywhere. during a restart, it is read in, but previous sims would not have set to proper value.

    char fnameProXYZ[100];
    sprintf(fnameProXYZ, "ComplexHistogram_Np%d.dat", params.numMolTypes);
    std::ofstream assemblyfile(fnameProXYZ);
    sprintf(fnameProXYZ, "MonomerDimerFile.dat");
    std::ofstream dimerfile(fnameProXYZ);
    sprintf(fnameProXYZ, "MeanComplexSize.dat");
    std::ofstream meanfile(fnameProXYZ);
    sprintf(fnameProXYZ, "GrowingComplexes.dat");
    std::ofstream growComplexFile(fnameProXYZ);
    sprintf(fnameProXYZ, "NboundPairFile.dat");
    std::ofstream pairOutfile(fnameProXYZ);
    sprintf(fnameProXYZ, "all_species.dat");
    std::ofstream speciesFile1(fnameProXYZ);
    //speciesFile1<<" Print to speciesFile1 \n"<<std::endl;
    // sprintf(fnameProXYZ, "all_species_check.dat");
    // std::ofstream speciesFile2(fnameProXYZ);
    // speciesFile2<<" Print to speciesFile2 \n"<<std::endl;

    int meanComplexSize { 0 };

    init_speciesFile(speciesFile1, counterArrays, molTemplateList, forwardRxns);
    init_counterCopyNums(counterArrays, moleculeList, molTemplateList, membraneObject); // works for default and restart
    
    
    write_all_species(0, speciesFile1, counterArrays);

    init_print_dimers(dimerfile, params, molTemplateList); // works for default and restart
    init_NboundPairs(counterArrays, pairOutfile, params, molTemplateList); // initializes to zero, re-calculated for a restart!!
    write_NboundPairs(counterArrays, pairOutfile, simItr, params);
    print_dimers(complexList, dimerfile, simItr, params, molTemplateList);
    //this will be wrong if there are no implicit lipids. 
    const int ILcopyIndex=moleculeList[implicitlipidIndex].interfaceList[0].index;

    meanComplexSize = print_complex_hist(complexList, assemblyfile, simItr, params, molTemplateList,counterArrays.copyNumSpecies[ILcopyIndex]);
    // TODO: TEMPORARY STUFF
    // Complex::obs = std::vector<int> { Molecule::numberOfMolecules, 0 };

    /*********************************/
    /*BEGIN SIMULATION*/
    /******************************/
    // TODO TEMP
    //    if (params.name != "") {
    //        std::string tmpName { params.name.substr(0, strlen(argv[0])) };
    //        strcpy(argv[0], tmpName.c_str());
    //    }
    std::cout << "*************** BEGIN SIMULATION **************** " << std::endl;

    // begin the timer
    MDTimer::time_point simulTimeStart = MDTimer::now();
    int numSavedDurations { 1000 };
    std::vector<std::chrono::duration<double>> durationList(numSavedDurations);
    std::fill(durationList.begin(), durationList.end(), std::chrono::duration<double>(simulTimeStart - totalTimeStart));
    
    unsigned DDTableIndex { 0 };
    /*Vectors to store binding probabilities for implicit lipids in 2D.*/
    std::vector<double> IL2DbindingVec {};
    std::vector<double> IL2DUnbindingVec {};
    std::vector<double> ILTableIDs {};
    
    if(params.fromRestart==true){
      auto endTime = MDTimer::now();
      auto endTimeFormat = MDTimer::to_time_t(endTime);
      std::ofstream restartFile { restartFileName, std::ios::out }; // to show different from append
      std::cout << "Writing restart file at iteration " << simItr;
      if(0 < strftime(charTime, sizeof(charTime), "%F %T", std::localtime(&endTimeFormat)) ) std::cout << charTime << '\n';
      //	<< ", system time: " << std::put_time(std::localtime(&endTimeFormat), "%F %T") << '\n';
      write_rng_state(); // write the current RNG state
      write_restart(simItr, restartFile, params, simulVolume, moleculeList, complexList, molTemplateList,
		    forwardRxns, backRxns, createDestructRxns, observablesList, emptyMolList, emptyComList, membraneObject);
      restartFile.close();
    }

    
    //    while (++simItr < params.nItr) {
    for (simItr += 1; simItr < params.nItr; ++simItr) {
        propCalled = 0;
	
        MDTimer::time_point startStep = MDTimer::now();
        
        // update the number of free implicit-lipids
        membraneObject.No_free_lipids = counterArrays.copyNumSpecies[moleculeList[implicitlipidIndex].interfaceList[0].index];
        

        // Zeroth order reactions (creation)
        check_for_zeroth_order_creation(simItr, params, emptyMolList, emptyComList, simulVolume, forwardRxns,
					createDestructRxns, moleculeList, complexList, molTemplateList, observablesList, counterArrays, membraneObject);

        // check for unimolecular reactions (including dissociation)
        check_for_unimolecular_reactions(simItr, params, emptyMolList, emptyComList, moleculeList, complexList,
					 simulVolume, forwardRxns, backRxns, createDestructRxns, molTemplateList, observablesList, counterArrays, membraneObject, IL2DbindingVec, IL2DUnbindingVec, ILTableIDs);
					 
        // Update member lists after creation and destruction

        simulVolume.update_memberMolLists(params, moleculeList, complexList, molTemplateList, membraneObject, simItr);

        // Measure separations between proteins in neighboring cells to identify all possible reactions.
        for (unsigned cellItr { 0 }; cellItr < simulVolume.subCellList.size(); ++cellItr) {
            for (unsigned memItr { 0 }; memItr < simulVolume.subCellList[cellItr].memberMolList.size(); ++memItr) {
                int targMolIndex { simulVolume.subCellList[cellItr].memberMolList[memItr] };        
                if (moleculeList[targMolIndex].isImplicitLipid)
                    continue;
                //Test bimolecular reactions, and binding to implicit-lipids
                if (moleculeList[targMolIndex].freelist.size() > 0) {
		    // first, check for implicit-lipid binding
		    int protype=moleculeList[targMolIndex].molTypeIndex;
		    if (molTemplateList[protype].bindToSurface==true){
			check_implicit_reactions(targMolIndex, implicitlipidIndex, simItr, tableIDs, DDTableIndex, params,
						 normMatrices, survMatrices, pirMatrices, moleculeList, complexList, molTemplateList,
						 forwardRxns, backRxns, counterArrays, membraneObject, IL2DbindingVec, IL2DUnbindingVec, ILTableIDs);
		    }
                    // secondly, loop over proteins in your same cell.
                    for (unsigned memItr2 { memItr + 1 };memItr2 < simulVolume.subCellList[cellItr].memberMolList.size(); ++memItr2) {
                        int partMolIndex { simulVolume.subCellList[cellItr].memberMolList[memItr2] };
                        check_bimolecular_reactions(targMolIndex, partMolIndex, simItr, tableIDs, DDTableIndex, params,
                                                    normMatrices, survMatrices, pirMatrices, moleculeList, complexList, molTemplateList,
						                                  forwardRxns, backRxns, counterArrays, membraneObject);
                    } // loop over protein partners in your same cell
                    // thirdly, loop over all neighboring cells, and all proteins in those cells.
                    // for PBC, all cells have maxnbor neighbor cells. For reflecting, edge have fewer.
                    for (auto& neighCellItr : simulVolume.subCellList[cellItr].neighborList) {
                        for (unsigned memItr2 { 0 };
                             memItr2 < simulVolume.subCellList[neighCellItr].memberMolList.size(); ++memItr2) {
                            int partMolIndex { simulVolume.subCellList[neighCellItr].memberMolList[memItr2] };
                            check_bimolecular_reactions(targMolIndex, partMolIndex, simItr, tableIDs, DDTableIndex,
                                                        params, normMatrices, survMatrices, pirMatrices, moleculeList, complexList,
							                                   molTemplateList, forwardRxns, backRxns, counterArrays, membraneObject);
                        } // loop over all proteins in this neighbor cell
                    } // loop over all neighbor cells
                } // if protein i is free to bind
            } // loop over all proteins in initial cell
        } // End looping over all cells.
        /*Now that separations and reaction probabilities are calculated,
          decide whether to perform reactions for each protein.
        */
        for (int molItr {0}; molItr < moleculeList.size(); ++molItr) {
            // only continue if the molecule actually exists, and isn't implicit-lipid
            if (moleculeList[molItr].isEmpty || moleculeList[molItr].isImplicitLipid)
                continue;
                
            //Skip any proteins that just dissociated during this time step
            if (moleculeList[molItr].crossbase.size() > 0) {
                /*Evaluate whether to perform a reaction with protein i, and with whom. Flag=1 means
                reaction is performed. Returns correct ci1 and ci2 for this rxn.
                Loop over all reactions individually, instead of summing probabilities
                */
                // these are indices in crossbase/mycrossint/crossrxn of the reaction for molecules 1 and 2,
                // should it occur
                int crossIndex1 { 0 };
                int crossIndex2 { 0 };
                bool willReact { determine_if_reaction_occurs(crossIndex1, crossIndex2, Constants::iRandMax, 
                                                              moleculeList[molItr], moleculeList,forwardRxns) };                                            
                if (params.debugParams.forceAssoc) {
                    willReact = false; // we chose an association reaction
                    crossIndex1 = 0;
                    int molItr2 = moleculeList[molItr].crossbase[crossIndex1]; // crosspart[p1][ci1];
                    double pmatch = moleculeList[molItr].probvec[crossIndex1];
                    if (pmatch > 0)
                        willReact = true;
                    if (!moleculeList[molItr].isImplicitLipid){
                      for (unsigned j = 0; j < moleculeList[molItr2].crossbase.size(); ++j) {
                        if (moleculeList[molItr2].probvec[j] == pmatch) {
                            crossIndex2 = j;
                            if (moleculeList[molItr].crossrxn[crossIndex1]
                                == moleculeList[molItr2].crossrxn[crossIndex2])
                                break;
                        }
                      }
                    }else{
                        crossIndex2 = 0;
                    }                    
                }
                if (willReact) {
                    /*This molecule will perform a bimolecular reaction
                      either physically associate two molecules into a complex (A+B->AB)
                      or change state of one (or both) reactants (A+B->A+B')
                     */

                    int molItr2 { moleculeList[molItr].crossbase[crossIndex1] };
                    int ifaceIndex1 { moleculeList[molItr].mycrossint[crossIndex1] };
                    int ifaceIndex2;
                    if(moleculeList[molItr2].isImplicitLipid==false){ 
                        ifaceIndex2 = moleculeList[molItr2].mycrossint[crossIndex2];
                    }else{
                    	   ifaceIndex2 = 0;
                    }
                    int comIndex1 { moleculeList[molItr].myComIndex };
                    int comIndex2 { moleculeList[molItr2].myComIndex };
                    std::array<int, 3> rxnIndex = moleculeList[molItr].crossrxn[crossIndex1];

		    
		    /*First if statement is to determine if reactants are physically associating*/
                    if (forwardRxns[rxnIndex[0]].rxnType == ReactionType::bimolecular) {
                    	
                      if (moleculeList[molItr2].isImplicitLipid){ 
                        std::cout << "Performing binding of molecules " << molItr << " to the membrane surface " << " ["
                                  << molTemplateList[moleculeList[molItr].molTypeIndex].molName << "("
                                  << molTemplateList[moleculeList[molItr].molTypeIndex].interfaceList[ifaceIndex1].name
                                  << ")] with [reaction, rate] = [" << rxnIndex[0] << ',' << rxnIndex[1]
                                  << "] Rate: "<<forwardRxns[rxnIndex[0]].rateList[rxnIndex[1]].rate<<" at iteration " << simItr << ".\n";
                        //complexList[comIndex1].display();
			std::cout <<" Complex 1 size: "<<complexList[comIndex1].memberList.size()<<"\n";
			std::cout <<"Implicit lipid, my Com Index: "<<comIndex2<<" size of memberlist: "<<complexList[comIndex2].memberList.size()<<std::endl;
			if (moleculeList[molItr].interfaceList[ifaceIndex1].index
                            == forwardRxns[rxnIndex[0]].reactantListNew[0].absIfaceIndex) {
			  
			  associate_binding_to_surface(ifaceIndex1, ifaceIndex2, moleculeList[molItr], moleculeList[molItr2],
						       complexList[comIndex1], complexList[comIndex2], params, forwardRxns[rxnIndex[0]],
						       moleculeList, molTemplateList, emptyMolList, emptyComList, observablesList,
						       counterArrays, complexList, membraneObject, forwardRxns, backRxns);
			}else{
			  //IL is listed first as the reactant.
			  associate_binding_to_surface(ifaceIndex2, ifaceIndex1, moleculeList[molItr2], moleculeList[molItr],
						       complexList[comIndex2], complexList[comIndex1], params, forwardRxns[rxnIndex[0]],
						       moleculeList, molTemplateList, emptyMolList, emptyComList, observablesList,
						       counterArrays, complexList, membraneObject, forwardRxns, backRxns);

			}
                      }         
                      if (moleculeList[molItr2].isImplicitLipid == false) {
                        std::cout << "Performing association between molecules " << molItr << " and " << molItr2 << " ["
                                  << molTemplateList[moleculeList[molItr].molTypeIndex].molName << "("
                                  << molTemplateList[moleculeList[molItr].molTypeIndex].interfaceList[ifaceIndex1].name
                                  << ") and " << molTemplateList[moleculeList[molItr2].molTypeIndex].molName << "("
                                  << molTemplateList[moleculeList[molItr2].molTypeIndex].interfaceList[ifaceIndex2].name
                                  << ")] with [reaction, rate] = [" << rxnIndex[0] << ',' << rxnIndex[1]
				  << "] Rate: "<<forwardRxns[rxnIndex[0]].rateList[rxnIndex[1]].rate<<" at iteration " << simItr << ".\n";          
                        //complexList[comIndex1].display();
                        //complexList[comIndex2].display();
			std::cout <<" Complex 1 size: "<<complexList[comIndex1].memberList.size()<<"\n";
			std::cout <<" Complex 2 size: "<<complexList[comIndex2].memberList.size()<<"\n";
			  
                        // For association, molecules must be read in in the order used to define the reaction parameters.
                        if (moleculeList[molItr].interfaceList[ifaceIndex1].index
                            == forwardRxns[rxnIndex[0]].reactantListNew[0].absIfaceIndex) {
                            associate(ifaceIndex1, ifaceIndex2, moleculeList[molItr], moleculeList[molItr2],
                                complexList[comIndex1], complexList[comIndex2], params, forwardRxns[rxnIndex[0]],
                                moleculeList, molTemplateList, emptyMolList, emptyComList, observablesList,
				      counterArrays, complexList, membraneObject,  forwardRxns, backRxns);
                        } else {
                            associate(ifaceIndex2, ifaceIndex1, moleculeList[molItr2], moleculeList[molItr],
                                complexList[comIndex2], complexList[comIndex1], params, forwardRxns[rxnIndex[0]],
                                moleculeList, molTemplateList, emptyMolList, emptyComList, observablesList,
				      counterArrays, complexList, membraneObject,  forwardRxns, backRxns);
                        }
                      }
                    }else if (forwardRxns[rxnIndex[0]].rxnType == ReactionType::biMolStateChange) {
                        //In this case, after two molecules collide, at least one of them changes state, rather than forming a complex. 
                        int facilMolIndex { molItr };
                        int facilIfaceIndex { ifaceIndex1 };
                        int facilComIndex { comIndex1 };
                        int stateMolIndex { molItr2 };
                        int stateComIndex { comIndex2 };
                        int stateIfaceIndex { ifaceIndex2 };
                        // figure out if the current molecule is the facilitator molecule or the molecule which has its
                        // interface state changed
                        if (moleculeList[molItr].molTypeIndex
                            != forwardRxns[rxnIndex[0]].reactantListNew[0].molTypeIndex) {
                            facilMolIndex = molItr2;
                            facilIfaceIndex = ifaceIndex2;
                            facilComIndex = comIndex2;
                            stateMolIndex = molItr;
                            stateComIndex = comIndex1;
                            stateIfaceIndex = ifaceIndex1;
                        }

                        std::cout << "Performing bimolecular state change on molecule " << stateMolIndex
                                  << " as facilitated by molecule " << facilMolIndex << " at iteration " << simItr
                                  << '\n';
                        perform_bimolecular_state_change(stateIfaceIndex, facilIfaceIndex, rxnIndex,
                            moleculeList[stateMolIndex], moleculeList[facilMolIndex], complexList[stateComIndex],
                            complexList[facilComIndex], counterArrays, params, forwardRxns, backRxns, moleculeList,
							 complexList, molTemplateList, observablesList, membraneObject);
                    } else {
                        std::cerr << "ERROR: Attemping bimolecular reaction which has no reaction type. Exiting..\n";
                        exit(1);
                    }
                } else {
                    /*No reaction was chosen for this molecule*/
                    if (moleculeList[molItr].trajStatus == TrajStatus::none) {
                        // Propagate complex with random translational and
                        // rotational motion
                        create_complex_propagation_vectors(params, complexList[moleculeList[molItr].myComIndex], moleculeList, 
                                                           complexList, molTemplateList, membraneObject);
                        for (auto& memMol : complexList[moleculeList[molItr].myComIndex].memberList)
                            moleculeList[memMol].trajStatus = TrajStatus::canBeResampled;
                    }
                    // Set probability of this protein to zero in all reactions so it doesn't try to
                    // react again but the partners still will avoid overlapping.
                    for (unsigned crossItr { 0 }; crossItr < moleculeList[molItr].crossbase.size(); ++crossItr) {
                        int skipMol { moleculeList[molItr].crossbase[crossItr] };
                        for (unsigned crossItr2 { 0 }; crossItr2 < moleculeList[skipMol].crossbase.size();
                             ++crossItr2) {
                            if (moleculeList[skipMol].crossbase[crossItr2] == moleculeList[molItr].index)
                                moleculeList[skipMol].probvec[crossItr2] = 0;
                        }
                    }
                }
                
            } else if (moleculeList[molItr].crossbase.size() == 0) {
                /*this protein has ncross=0,
                meaning it neither dissociated nor tried to associate.
                however, it could have movestat=2 if it is part of a multi-protein
                complex that already displaced.
                */
                if (moleculeList[molItr].trajStatus == TrajStatus::none) {
                    create_complex_propagation_vectors(params, complexList[moleculeList[molItr].myComIndex], moleculeList, 
                                                       complexList, molTemplateList, membraneObject);
                    for (auto& memMol : complexList[moleculeList[molItr].myComIndex].memberList)
                        moleculeList[memMol].trajStatus = TrajStatus::canBeResampled;
                }
            }
        } // done testing all molecules for bimolecular reactions
        /*
        // now we carry out propagation
        for (auto& mol : moleculeList){      	       
            if (mol.isEmpty || mol.isImplicitLipid || mol.trajStatus == TrajStatus::propagated)
                continue;
            create_complex_propagation_vectors(params,complexList[mol.myComIndex],moleculeList,complexList,molTemplateList,
                                               membraneObject);
            complexList[mol.myComIndex].propagate(moleculeList);
        }
        */
        // Now we have to check for overlap!!!
        for (auto& mol : moleculeList) {
            //Now track each complex (ncrosscom), and test for overlap of all proteins in that complex before
            //performing final position updates.
            if (mol.isEmpty || mol.isImplicitLipid || mol.trajStatus == TrajStatus::propagated)
                continue;

            int c1 { mol.myComIndex };
            // if (ncrosscom[moleculeList[i].mycomplex] > 0) {
            if (complexList[c1].ncross > 0) {
                if (mol.trajStatus == TrajStatus::none || mol.trajStatus == TrajStatus::canBeResampled) {
                    //For any protein that overlapped and did not react, check whether it overlaps with its partners,
                    //  do all proteins in the same complex at the same time.
                    //  Also, if both proteins are stuck to membrane, only do xy displacement, ignore z
                    //  TODO: Maybe do a boundary sphere overlap check first?
                    
                    if (complexList[c1].D.z < 1E-10) {
                        sweep_separation_complex_rot_memtest(
							     simItr, mol.index, params, moleculeList, complexList, forwardRxns, molTemplateList, membraneObject);
                    } else {
                        sweep_separation_complex_rot(
						        simItr, mol.index, params, moleculeList, complexList, forwardRxns, molTemplateList, membraneObject);
                    }
                }
            } else {
                if (mol.trajStatus == TrajStatus::none || mol.trajStatus == TrajStatus::canBeResampled) {
                    //For proteins with ncross=0, they either moved independently, or their displacements
                    //were selected based on the complex they were part of, and they may not yet been moved.
                     
                    std::array<double, 9> M = create_euler_rotation_matrix(complexList[c1].trajRot);
                    reflect_traj_complex_rad_rot(params, moleculeList, complexList[c1], M, membraneObject);
                    complexList[c1].propagate(moleculeList);
                }
            }
        }
        
        if (simItr % params.trajWrite == 0) {
            auto endTime = MDTimer::now();
            auto endTimeFormat = MDTimer::to_time_t(endTime);
            std::ofstream restartFile { restartFileName, std::ios::out }; // to show different from append
            std::cout << "Writing restart file at iteration " << simItr;
	    //                      << ", system time: " << std::put_time(std::localtime(&endTimeFormat), "%F %T") << '\n';
            if(0 < strftime(charTime, sizeof(charTime), "%F %T", std::localtime(&endTimeFormat)) ) std::cout << charTime << '\n';
	    write_rng_state(); // write the current RNG state
            write_restart(simItr, restartFile, params, simulVolume, moleculeList, complexList, molTemplateList,
                          forwardRxns, backRxns, createDestructRxns, observablesList, emptyMolList, emptyComList, membraneObject);
            restartFile.close();

            std::cout << "Writing trajectory..." << '\n';
            std::ofstream trajFile { trajFileName, std::ios::app }; // for append
            write_traj(simItr, trajFile, params, moleculeList, molTemplateList , membraneObject);
            trajFile.close();
            // write_complex_components(simItr, complexFile, params, moleculeList, complexList, molTemplateList);

            if (params.pdbWrite != -1) {
                std::cout << "Writing PDB file for current frame.\n";
                write_pdb(simItr, simItr, params, moleculeList, molTemplateList, membraneObject);
            }
        }

        // Clear lists used for reweighting and encounter tracking
        for (auto& oneMol : moleculeList) {
            if (oneMol.isEmpty || oneMol.isImplicitLipid)
                continue;

            clear_reweight_vecs(oneMol);
            oneMol.trajStatus = TrajStatus::none;
            oneMol.crossbase.clear();
            oneMol.mycrossint.clear();
            oneMol.crossrxn.clear();
            oneMol.probvec.clear();

            // update complexes. if the complex has more than one member, it'll be updated
            // more than once, but this is probably more efficient than iterating over the complexes afterwards
            complexList[oneMol.myComIndex].ncross = 0;
            complexList[oneMol.myComIndex].trajStatus = TrajStatus::none;
        }

        using duration = std::chrono::duration<double>;
        durationList.erase(durationList.begin());
        durationList.emplace_back(MDTimer::now() - startStep);
        if (simItr % params.timeWrite == 0) {
            double timeSimulated { simItr * params.timeStep };
            std::cout << linebreak;
            std::cout << "End iteration: " << simItr << ", simulation time: ";
            if (timeSimulated / 100000 < 1)
                std::cout << timeSimulated << " microseconds.\n";
            else
                std::cout << timeSimulated / 1E6 << " seconds.\n";
            // Write out N bound pairs, histogram of complex compositions, and monomer/dimer counts.
            write_NboundPairs(counterArrays, pairOutfile, simItr, params);
            //////////////////////////////////////////////////////////////
            print_dimers(complexList, dimerfile, simItr, params, molTemplateList);
            //////////////////////////////////////////////////////////////
            meanComplexSize = print_complex_hist(complexList, assemblyfile, simItr, params, molTemplateList, counterArrays.copyNumSpecies[ILcopyIndex]);
            auto endTime = MDTimer::now();
            auto endTimeFormat = MDTimer::to_time_t(endTime);
            std::cout << "System time: ";
	    // << std::put_time(std::localtime(&endTimeFormat), "%F %T") << '\n';
	    if(0 < strftime(charTime, sizeof(charTime), "%F %T", std::localtime(&endTimeFormat)) ) std::cout << charTime << '\n';
            std::cout << "Elapsed time: "
                      << std::chrono::duration_cast<std::chrono::minutes>(MDTimer::now() - totalTimeStart).count()
                      << " minutes\n";

            std::cout << "Number of molecules: " << Molecule::numberOfMolecules << '\n';
            std::cout << "Number of complexes: " << Complex::numberOfComplexes << '\n';
            std::cout << "Total reaction matches: " << totMatches << '\n';
            if (params.debugParams.printSystemInfo) {
                std::cout << "Printing full system information...\n";
                std::ofstream systemInfoFile { "system_information.dat", std::ios::app };
                print_system_information(simItr, systemInfoFile, moleculeList, complexList, molTemplateList);
                systemInfoFile.close();
            }
            // write observables
            if (!observablesList.empty()) {
                std::cout << "Writing observables to file...\n";
                std::ofstream observablesFile { observablesFileName, std::ios::app };
                write_observables(simItr, observablesFile, observablesList);
                observablesFile.close();
            }
            // write all species

            //std::ofstream speciesFile{ speciesFileName, std::ios::app };
            write_all_species(simItr, speciesFile1, counterArrays);
            //speciesFile.close();

            // Estimate time remaining
            duration avgTimeStepDuration
                = std::accumulate(durationList.begin(), durationList.end(), duration { 0 }) / numSavedDurations;
            duration timeLeft = (params.nItr - simItr) * avgTimeStepDuration;
            std::cout << "Avg timestep duration: " << avgTimeStepDuration.count()
                      << ", iterations remaining: " << params.nItr - simItr
                      << ", Time left: " << std::chrono::duration_cast<std::chrono::minutes>(timeLeft).count()
                      << " minutes\n";
            auto estTimeLeft = std::chrono::time_point_cast<std::chrono::seconds>(MDTimer::now() + timeLeft);
            auto estTimeEnd = std::chrono::system_clock::to_time_t(estTimeLeft);
            std::cout << "Estimated end time: ";
	    //<< std::put_time(std::localtime(&estTimeEnd), "%F %T") << '\n';
	    if(0 < strftime(charTime, sizeof(charTime), "%F %T", std::localtime(&estTimeEnd)) ) std::cout << charTime << '\n';
            std::cout << llinebreak;
        }
    } // end iterating over time steps
    
    // Write files at last timestep
    {
    
        std::cout << "Writing restart file at final iteration\n.";
        std::ofstream restartFile { restartFileName, std::ios::out }; // to show different from append
        write_rng_state(); // write the current RNG state
        write_restart(simItr, restartFile, params, simulVolume, moleculeList, complexList, molTemplateList, forwardRxns,
                      backRxns, createDestructRxns, observablesList, emptyMolList, emptyComList, membraneObject);
        restartFile.close();

        std::cout << "Writing trajectory..." << '\n';
        std::ofstream trajFile { trajFileName, std::ios::app }; // for append
        write_traj(simItr, trajFile, params, moleculeList, molTemplateList, membraneObject);
        trajFile.close();

        std::cout << "Writing final configuration...\n";
        write_xyz("final_coords.xyz", params, moleculeList, molTemplateList);

        if (params.pdbWrite != -1) {
            std::cout << "Writing PDB file for current frame.\n";
            write_pdb(simItr, simItr, params, moleculeList, molTemplateList, membraneObject);
        }

        if (params.debugParams.printSystemInfo) {
            std::cout << "Printing full system information...\n";
            std::ofstream systemInfoFile { "system_information.dat", std::ios::app };
            print_system_information(simItr, systemInfoFile, moleculeList, complexList, molTemplateList);
            systemInfoFile.close();
        }

        // write observables
        if (!observablesList.empty()) {
            std::cout << "Writing observables to file...\n";
            std::ofstream observablesFile { observablesFileName, std::ios::app };
            write_observables(simItr, observablesFile, observablesList);
            observablesFile.close();
        }
         
        // write all species
        //std::ofstream speciesFile { speciesFileName, std::ios::app };
        write_all_species(simItr, speciesFile1, counterArrays);
        //speciesFile.close();
	// Write out N bound pairs, histogram of complex compositions, and monomer/dimer counts.
	write_NboundPairs(counterArrays, pairOutfile, simItr, params);
	print_dimers(complexList, dimerfile, simItr, params, molTemplateList);
	meanComplexSize = print_complex_hist(complexList, assemblyfile, simItr, params, molTemplateList, counterArrays.copyNumSpecies[ILcopyIndex]);

    }

    /*Write out final result*/
    std::cout << llinebreak << "End simulation\n";
    auto endTime = MDTimer::now();
    auto endTimeFormat = MDTimer::to_time_t(endTime);
    std::cout << "End date: ";
    //<< std::put_time(std::localtime(&endTimeFormat), "%F %T") << '\n';
    if(0 < strftime(charTime, sizeof(charTime), "%F %T", std::localtime(&endTimeFormat)) ) std::cout << charTime << '\n';
    std::chrono::duration<double> wallTime = endTime - totalTimeStart;
    std::cout << "\tWall Time: ";
    //    if (wallTime.count() / 60 < 1)
    std::cout << wallTime.count() << " seconds\n";
    //    else
    //        std::cout << std::chrono::duration_cast<std::chrono::minutes>(wallTime).count() << " minutes\n";

    delete[] tableIDs;
} // end main