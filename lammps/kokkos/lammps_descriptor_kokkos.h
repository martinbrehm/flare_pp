#ifndef LAMMPS_DESCRIPTOR_KOKKOS_H
#define LAMMPS_DESCRIPTOR_KOKKOS_H

#include <Eigen/Dense>
#include <functional>
#include <vector>
#include <Kokkos_Core.hpp>

// TODO: Change types if memory layouts change for some tensors
template<class MemberType, class ViewType1D, class ViewType3D, class TypeType, class NeighType>
KOKKOS_INLINE_FUNCTION
void single_bond_kokkos(
    int i,
    MemberType &team_member,
    ViewType1D &single_bond_vals,
    ViewType3D &single_bond_env_dervs,
    ViewType3D &g, ViewType3D &Y,
    TypeType &type,
    int n_species, int n_max, int n_harmonics, NeighType d_neighbors_short, int n_neighs, int neighmask
    ) {

  // Initialize vectors.
  int n_radial = n_species * n_max;
  int n_bond = n_radial * n_harmonics;

  Kokkos::parallel_for(Kokkos::TeamVectorRange(team_member, n_bond), [&](int &d){
      single_bond_vals(d) = 0;
  });

  // Kokkos::deep_copy(single_bond_env_dervs, 0.0);
  Kokkos::parallel_for(Kokkos::TeamThreadRange(team_member, n_bond), [&] (int &d){
    Kokkos::parallel_for(Kokkos::ThreadVectorRange(team_member, 3*n_neighs), [&] (int &k){
        int j = k / 3;
        int c = k - 3*j;
        single_bond_env_dervs(d,j,c) = 0.0;
    });
  });
  team_member.team_barrier();

  Kokkos::parallel_for(Kokkos::TeamThreadRange(team_member, n_max*n_harmonics), [&](int &nlm){


      int radial_counter = nlm/n_harmonics;
      int angular_counter = nlm - n_harmonics*radial_counter;

      // TODO: parallelize over components
      Kokkos::parallel_for(Kokkos::ThreadVectorRange(team_member, n_neighs), [&] (int &jj){
          int j = d_neighbors_short(i,jj);
          j &= neighmask;
          int s = type[j] - 1;
          int descriptor_counter = s * n_max * n_harmonics + nlm;

          // Retrieve radial values.
          double g_val = g(jj,radial_counter,0);
          double gx_val = g(jj,radial_counter,1);
          double gy_val = g(jj,radial_counter,2);
          double gz_val = g(jj,radial_counter,3);


          double h_val = Y(jj,angular_counter,0);
          double hx_val = Y(jj,angular_counter,1);
          double hy_val = Y(jj,angular_counter,2);
          double hz_val = Y(jj,angular_counter,3);

          double bond = g_val * h_val;
          double bond_x = gx_val * h_val + g_val * hx_val;
          double bond_y = gy_val * h_val + g_val * hy_val;
          double bond_z = gz_val * h_val + g_val * hz_val;

          // Update single bond basis arrays.
          Kokkos::atomic_add(&single_bond_vals(descriptor_counter),bond); // TODO: bad
          //single_bond_vals(descriptor_counter) += bond;

          single_bond_env_dervs(descriptor_counter,jj,0) = bond_x;
          single_bond_env_dervs(descriptor_counter,jj,1) = bond_y;
          single_bond_env_dervs(descriptor_counter,jj,2) = bond_z;
          //printf("i = %d, j = %d, n = %d, lm = %d, idx = %d, bond = %g %g %g %g\n", i, j, radial_counter, angular_counter, descriptor_counter, bond, bond_x, bond_y, bond_z);
      });
  });
  /*
  Kokkos::single(Kokkos::PerTeam(team_member), [&](){
    printf("i = %d, d =", i);
    for(int d = 0; d < n_bond; d++){
      printf(" %g", single_bond_vals(d));
    }
    printf("\n");
  });
  */
}

template<class MemberType, class ScratchView1D, class ScratchView3D>
KOKKOS_INLINE_FUNCTION
void B2_descriptor_kokkos(MemberType &team_member, ScratchView1D &B2_vals, ScratchView3D &B2_env_dervs,
                   const ScratchView1D &single_bond_vals,
                   const ScratchView3D &single_bond_env_dervs, int n_species,
                   int n_max, int l_max, int n_neighs) {

  int n_radial = n_species * n_max;
  int n_harmonics = (l_max + 1) * (l_max + 1);
  int n_descriptors = (n_radial * (n_radial + 1) / 2) * (l_max + 1);
  double np12 = n_radial + 0.5;

  //Kokkos::deep_copy(B2_env_dervs,0.0);
  Kokkos::parallel_for(Kokkos::TeamThreadRange(team_member, n_descriptors), [&] (int &d){
    Kokkos::parallel_for(Kokkos::ThreadVectorRange(team_member, 3*n_neighs), [&] (int &k){
        int j = k / 3;
        int c = k - 3*j;
        B2_env_dervs(j,c,d) = 0.0;
    });
  });
  team_member.team_barrier();

  Kokkos::parallel_for(Kokkos::TeamThreadRange(team_member, n_descriptors), [&] (int &nnl){

      int x = nnl/(l_max+1);
      int l = nnl-x*(l_max+1);

      int n1 = -std::sqrt(np12*np12 - 2*x) + np12;
      int n2 = x - n1*(np12 - 1 - 0.5*n1);

      double B2_val = 0.0;

      // This loop is way too small for parallel efficiency
      Kokkos::parallel_reduce(Kokkos::ThreadVectorRange(team_member, 2*l+1), [&] (int &m, double &B2_val){
          int n1_l = n1 * n_harmonics + (l * l + m);
          int n2_l = n2 * n_harmonics + (l * l + m);
          B2_val += single_bond_vals(n1_l) * single_bond_vals(n2_l);
      }, B2_val);

      //printf(" | n1 = %d, n2 = %d, l = %d, B2 = %g |\n", n1, n2, l, B2_val);
      B2_vals(nnl) = B2_val;

      Kokkos::parallel_for(Kokkos::ThreadVectorRange(team_member, 3*n_neighs), [&] (int &jjc){
          int atom_index = jjc/3;
          int comp = jjc - 3*atom_index;

          B2_env_dervs(atom_index, comp,nnl) = 0;
      });

      // TODO: Check if ThreadVector launch is bad
      // (I think this approach does better w.r.t. memory access
      for(int m = 0; m < 2*l+1; m++){
          int n1_l = n1 * n_harmonics + (l * l + m);
          int n2_l = n2 * n_harmonics + (l * l + m);

          double single_1 = single_bond_vals(n1_l);
          double single_2 = single_bond_vals(n2_l);

          Kokkos::parallel_for(Kokkos::ThreadVectorRange(team_member, 3*n_neighs), [&] (int &jjc){
              int atom_index = jjc/3;
              int comp = jjc - 3*atom_index;

              B2_env_dervs(atom_index, comp, nnl) +=
                single_1 * single_bond_env_dervs(n2_l, atom_index, comp)
                + single_bond_env_dervs(n1_l, atom_index, comp) * single_2;
          });
      }
  });
}

#endif
