#include "reactions/bimolecular/bimolecular_reactions.hpp"

bool get_distance_to_surface(int pro1, int pro2, int iface1, int iface2, int rxnIndex, int rateIndex, bool isStateChangeBackRxn,
    double& sep, double& R1, double Rmax, std::vector<Complex>& complexList, const ForwardRxn& currRxn,
    std::vector<Molecule>& moleculeList, const Membrane &membraneObject)
{
    double dx = moleculeList[pro1].interfaceList[iface1].coord.x;
    double dy = moleculeList[pro1].interfaceList[iface1].coord.y;
    double dz;
    if(complexList[moleculeList[pro1].myComIndex].D.z == 0)
           dz = 0;
    else
           dz = moleculeList[pro1].interfaceList[iface1].coord.z - (-membraneObject.waterBox.z / 2.0);

    R1 = sqrt( (dz*dz) );
    sep = R1;

    /*Rmax should be the binding radius plus ~max diffusion distance, using 3*sqrt(6*Dtot*deltat)*/
    if (R1 < Rmax) {
    	  /*in this case we evaluate the probability of this reaction*/
        moleculeList[pro1].crossbase.push_back(pro2);
        moleculeList[pro1].mycrossint.push_back(iface1);
        moleculeList[pro1].crossrxn.push_back(std::array<int, 3> { rxnIndex, rateIndex, isStateChangeBackRxn });
        ++complexList[moleculeList[pro1].myComIndex].ncross;
        return true;
    }
    return false;
}