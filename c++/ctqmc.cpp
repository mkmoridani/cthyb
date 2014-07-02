/*******************************************************************************
 *
 * TRIQS: a Toolbox for Research in Interacting Quantum Systems
 *
 * Copyright (C) 2013, P. Seth, I. Krivenko, M. Ferrero and O. Parcollet
 *
 * TRIQS is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * TRIQS is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * TRIQS. If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/
#include "ctqmc.hpp"
#include <triqs/utility/callbacks.hpp>
#include <triqs/utility/exceptions.hpp>
#include <triqs/gfs.hpp>

#include "move_insert.hpp"
#include "move_remove.hpp"
#include "measure_g.hpp"
#include "measure_perturbation_hist.hpp"
//DEBUG
#include <triqs/h5.hpp>

namespace cthyb {

ctqmc::ctqmc(double beta_, std::map<std::string,std::vector<int>> const & gf_struct_, int n_iw, int n_tau):
  beta(beta_), gf_struct(gf_struct_) {

  if ( n_tau <= 2*n_iw ) TRIQS_RUNTIME_ERROR << "Must use as least twice as many tau points as Matsubara frequencies: n_iw = " << n_iw << " but n_tau = " << n_tau << ".";

  std::vector<std::string> block_names;
  std::vector<gf<imfreq>> g0_iw_blocks;
  std::vector<gf<imtime>> g_tau_blocks;
  std::vector<gf<imtime>> delta_tau_blocks;

  for (auto const& block : gf_struct) {
    block_names.push_back(block.first);
    int n = block.second.size();
    g0_iw_blocks.push_back(gf<imfreq>{{beta, Fermion, n_iw}, {n, n}});
    g_tau_blocks.push_back(gf<imtime>{{beta, Fermion, n_tau, half_bins}, {n, n}});
    delta_tau_blocks.push_back(gf<imtime>{{beta, Fermion, n_tau, half_bins}, {n, n}});
  }

  G0_iw = make_block_gf(block_names, g0_iw_blocks);
  G_tau = make_block_gf(block_names, g_tau_blocks);
  Delta_tau = make_block_gf(block_names, delta_tau_blocks);

}

// TODO Move functions below to triqs library
template<typename F, typename T>
std::vector<std14::result_of_t<F(T)>> map(F && f, std::vector<T> const & V) {
  std::vector<std14::result_of_t<F(T)>> res;
  res.reserve(V.size());
  for(auto & x : V) res.emplace_back(f(x));
  return res;
}

template<typename F, typename G>
gf<block_index,std14::result_of_t<F(G)>> map(F && f, gf<block_index,G> const & g) {
  return make_block_gf(map(f, g.data()));
}

void ctqmc::solve(real_operator_t h_loc, params::parameters params,
                  std::vector<real_operator_t> const & quantum_numbers, bool use_quantum_numbers) {

  // determine basis of operators to use
  fundamental_operator_set fops;
  std::map<std::pair<int,int>,int> linindex;
  int block_index = 0;
  for (auto const & b: gf_struct) {
    int inner_index = 0;
    for (auto const & a: b.second) {
      fops.insert(b.first, a);
      linindex[std::make_pair(block_index, inner_index)] = fops[{b.first,a}];
      inner_index++;
    }
    block_index++;
  }

  // Calculate imfreq quantities
  auto G0_iw_inv = map([](gf_const_view<imfreq> x){return triqs::gfs::inverse(x);}, G0_iw);
  auto Delta_iw = G0_iw_inv;

  // Add quadratic terms to h_loc
  int b = 0;
  for (auto const & bl: gf_struct) {
    for (auto const & a1: bl.second) {
      for (auto const & a2: bl.second) {
        h_loc = h_loc + G0_iw[b].singularity()(2)(a1,a2).real() * c_dag(bl.first,a1)*c(bl.first,a2);
      }
    }
    b++;
  }

  // Determine terms Delta_iw from G0_iw and ensure that the 1/iw behaviour of G0_iw is correct
  b = 0;
  triqs::clef::placeholder<0> iw_;
  for (auto const & bl: gf_struct) {
    Delta_iw[b](iw_) << G0_iw_inv[b].singularity()(-1)*iw_ + G0_iw_inv[b].singularity()(0);
    Delta_iw[b] = Delta_iw[b] - G0_iw_inv[b];
    Delta_tau[b]() = inverse_fourier(Delta_iw[b]); 
    G0_iw[b](iw_) << iw_ + G0_iw_inv[b].singularity()(0) ;
    G0_iw[b] = G0_iw[b] - Delta_iw[b];
    G0_iw[b]() = triqs::gfs::inverse(G0_iw[b]);
    b++;
  }

// DEBUG
//  Delta_tau = map([](gf_const_view<imfreq> x){return make_gf_from_inverse_fourier(x);}, Delta_iw);
//auto delta_tau2 = make_gf_from_inverse_fourier(Delta_iw[0]);
//std::cout << "h_loc = " << h_loc << std::endl;
//std::cout << "G0_iw tail= " << G0_iw[0].singularity() << std::endl;
//std::cout << "Delta_iw tail= " << Delta_iw[0].singularity() << std::endl;
//std::cout << "G0_iw = " << G0_iw[0].data()(range(0,10),range(),range()) << std::endl;
//std::cout << "Delta_tau = " << Delta_tau[0].data()(range(0,10),range(),range()) << std::endl;
//std::cout << "Delta_tau = " << Delta_tau[0].data() << std::endl;
//std::cout << "Deltaw = " << Deltaw[0].data()(range(0,10),range(),range()) << std::endl;
//std::cout << "delta_tau2 = " << delta_tau2.data()(range(0,10),range(),range()) << std::endl;

// DEBUG
// Set hybridization function
//double V = 1.0;
//double epsilon = 2.3;
//triqs::clef::placeholder<0> om_;
//auto delta_w = gf<imfreq>{{beta, Fermion}, {2,2}};
//delta_w(om_) << V*V / (om_ - epsilon) + V*V / (om_ + epsilon);
//Delta_tau[0]() = triqs::gfs::inverse_fourier(delta_w);
// MORE DEBUG
//triqs::h5::file G_file("kondo_anderson.h5",H5F_ACC_RDONLY);
//triqs::h5::group gp(G_file);
//h5_read(G_file,"delta_tot",Delta_tau[0]);//Delta_tau_view()[0]);

  // Determine block structure
  if (use_quantum_numbers)
   sosp = {h_loc, quantum_numbers, fops};
  else 
   sosp = {h_loc, fops};

  qmc_data data(beta, params, sosp, linindex, Delta_tau);
  mc_tools::mc_generic<mc_sign_type> qmc(params);
 
  // Moves
  auto& delta_names = Delta_tau.domain().names();
  for (size_t block = 0; block < Delta_tau.domain().size(); ++block) {
   int block_size = Delta_tau[block].data().shape()[1];
   qmc.add_move(move_insert_c_cdag(block, block_size, data, qmc.rng(), false), "Insert Delta_" + delta_names[block]);
   qmc.add_move(move_remove_c_cdag(block, block_size, data, qmc.rng()), "Remove Delta_" + delta_names[block]);
  }
 
  // Measurements
  if (params["measure_g_tau"]) {
   auto& g_names = G_tau.domain().names();
   for (size_t block = 0; block < G_tau.domain().size(); ++block) {
    qmc.add_measure(measure_g(block, G_tau[block], data), "G measure (" + g_names[block] + ")");
   }
  }
  if (params["measure_pert_order"]) {
   auto& g_names = G_tau.domain().names();
   for (size_t block = 0; block < G_tau.domain().size(); ++block) {
    qmc.add_measure(measure_perturbation_hist(block, data, "histo_pert_order_" + g_names[block] + ".dat"), "Perturbation order (" + g_names[block] + ")");
   }
  }
 
  // run!! The empty configuration has sign = 1
  qmc.start(1.0, triqs::utility::clock_callback(params["max_time"]));
  qmc.collect_results(_comm);
}

//----------------------------------------------------------------------------------------------

parameters_t ctqmc::solve_parameters() {

 auto pdef = parameters_t{};
 boost::mpi::communicator world;

 pdef.add_field("n_cycles", no_default<int>(), "Number of QMC cycles")
     .add_field("length_cycle", int(50), "Length of a single QMC cycle")
     .add_field("n_warmup_cycles", int(5000), "Number of cycles for thermalization")
     .add_field("random_seed", int(34788 + 928374 * world.rank()), "Seed for random number generator")
     .add_field("random_name", std::string(""), "Name of random number generator")
     .add_field("max_time", int(-1), "Maximum runtime in seconds, use -1 to set infinite")
     .add_field("verbosity", (world.rank()==0 ? int(3) : int(0)), "Verbosity level")
     .add_field("use_trace_estimator", bool(false), "Calculate the full trace or use an estimate?")
     .add_field("measure_g_tau", bool(true), "Whether to measure G(tau)")
     .add_field("measure_pert_order", bool(false), "Whether to measure perturbation order")
     .add_field("make_histograms", bool(false), "Make the analysis histograms of the trace computation");

 return pdef;
}

//----------------------------------------------------------------------------------------------

void ctqmc::help() {
 // TODO
}

}
