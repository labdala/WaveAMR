/* ---------------------------------------------------------------------
 *
 * Copyright (C) 2006 - 2020 by the deal.II authors
 *
 * This file is part of the deal.II library.
 *
 * The deal.II library is free software; you can use it, redistribute
 * it, and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * The full text of the license can be found in the file LICENSE.md at
 * the top level directory of deal.II.
 *
 * ---------------------------------------------------------------------

 *
 * Author: Wolfgang Bangerth, Texas A&M University, 2006
 */

// @sect3{Include files}

// We start with the usual assortment of include files that we've seen in so
// many of the previous tests:
#include <deal.II/base/function.h>
#include <deal.II/base/quadrature_lib.h>

#include <deal.II/lac/affine_constraints.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/vector.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_simplex_p.h>
#include <deal.II/fe/mapping_fe.h>

#include <deal.II/numerics/data_out.h>

#include <fstream>
#include <iostream>

// Here are the only three include files of some new interest: The first one
// is already used, for example, for the
// VectorTools::interpolate_boundary_values and
// MatrixTools::apply_boundary_values functions. However, we here use another
// function in that class, VectorTools::project to compute our initial values
// as the $L^2$ projection of the continuous initial values. Furthermore, we
// use VectorTools::create_right_hand_side to generate the integrals
// $(f^n,\phi^n_i)$. These were previously always generated by hand in
// <code>assemble_system</code> or similar functions in application
// code. However, we're too lazy to do that here, so simply use a library
// function:
#include <deal.II/numerics/vector_tools.h>

// In a very similar vein, we are also too lazy to write the code to assemble
// mass and Laplace matrices, although it would have only taken copying the
// relevant code from any number of previous tutorial programs. Rather, we
// want to focus on the things that are truly new to this program and
// therefore use the MatrixCreator::create_mass_matrix and
// MatrixCreator::create_laplace_matrix functions. They are declared here:
#include <deal.II/numerics/matrix_tools.h>

// Finally, here is an include file that contains all sorts of tool functions
// that one sometimes needs. In particular, we need the
// Utilities::int_to_string class that, given an integer argument, returns a
// string representation of it. It is particularly useful since it allows for
// a second parameter indicating the number of digits to which we want the
// result padded with leading zeros. We will use this to write output files
// that have the form <code>solution-XXX.vtu</code> where <code>XXX</code>
// denotes the number of the time step and always consists of three digits
// even if we are still in the single or double digit time steps.
#include <deal.II/base/utilities.h>

// AMR
#include <deal.II/grid/grid_refinement.h>
#include <deal.II/numerics/error_estimator.h>
#include <deal.II/numerics/solution_transfer.h>

#include <chrono>

// The last step is as in all previous programs:
namespace Step23 {
using namespace dealii;

// @sect3{The <code>WaveEquation</code> class}

// Next comes the declaration of the main class. It's public interface of
// functions is like in most of the other tutorial programs. Worth
// mentioning is that we now have to store four matrices instead of one: the
// mass matrix $M$, the Laplace matrix $A$, the matrix $M+k^2\theta^2A$ used
// for solving for $U^n$, and a copy of the mass matrix with boundary
// conditions applied used for solving for $V^n$. Note that it is a bit
// wasteful to have an additional copy of the mass matrix around. We will
// discuss strategies for how to avoid this in the section on possible
// improvements.
//
// Likewise, we need solution vectors for $U^n,V^n$ as well as for the
// corresponding vectors at the previous time step, $U^{n-1},V^{n-1}$. The
// <code>system_rhs</code> will be used for whatever right hand side vector
// we have when solving one of the two linear systems in each time
// step. These will be solved in the two functions <code>solve_u</code> and
// <code>solve_v</code>.
//
// Finally, the variable <code>theta</code> is used to indicate the
// parameter $\theta$ that is used to define which time stepping scheme to
// use, as explained in the introduction. The rest is self-explanatory.
template <int dim> class WaveEquation {
public:
  WaveEquation();
  void run();

private:
  void setup_system();
  void solve_u();
  void solve_v();
  void refine_mesh(const unsigned int min_grid_level,
                   const unsigned int max_grid_level);
  void output_results() const;

  Triangulation<dim> triangulation;
  Triangulation<dim> Th;
  FE_Q<dim> fe;
  //  FE_SimplexP<dim> fe;
  DoFHandler<dim> dof_handler;

  AffineConstraints<double> constraints;

  SparsityPattern sparsity_pattern;
  SparseMatrix<double> mass_matrix;
  SparseMatrix<double> laplace_matrix;
  SparseMatrix<double> matrix_u;
  SparseMatrix<double> matrix_v;

  Vector<double> solution_u, solution_v;
  Vector<double> old_solution_u, old_solution_v;
  Vector<double> system_rhs;

  double time_step;
  double time;
  unsigned int timestep_number;
  const double theta;
};

// @sect3{Equation data}

// Before we go on filling in the details of the main class, let us define
// the equation data corresponding to the problem, i.e. initial and boundary
// values for both the solution $u$ and its time derivative $v$, as well as
// a right hand side class. We do so using classes derived from the Function
// class template that has been used many times before, so the following
// should not be a surprise.
//
// Let's start with initial values and choose zero for both the value $u$ as
// well as its time derivative, the velocity $v$:
template <int dim> class InitialValuesU : public Function<dim> {
public:
  virtual double value(const Point<dim> & /*p*/,
                       const unsigned int component = 0) const override {
    (void)component;
    Assert(component == 0, ExcIndexRange(component, 0, 1));
    return 0;
  }
};

template <int dim> class InitialValuesV : public Function<dim> {
public:
  virtual double value(const Point<dim> & /*p*/,
                       const unsigned int component = 0) const override {
    (void)component;
    Assert(component == 0, ExcIndexRange(component, 0, 1));
    return 0;
  }
};

// Secondly, we have the right hand side forcing term. Boring as we are, we
// choose zero here as well:
template <int dim> class RightHandSide : public Function<dim> {
public:
  virtual double value(const Point<dim> & /*p*/,
                       const unsigned int component = 0) const override {
    (void)component;
    Assert(component == 0, ExcIndexRange(component, 0, 1));
    return 0;
  }
};

// Finally, we have boundary values for $u$ and $v$. They are as described
// in the introduction, one being the time derivative of the other:
template <int dim> class BoundaryValuesU : public Function<dim> {
public:
  virtual double value(const Point<dim> &p,
                       const unsigned int component = 0) const override {
    (void)component;
    Assert(component == 0, ExcIndexRange(component, 0, 1));

    if ((this->get_time() <= 0.5) && (p[0] < 0) && (p[1] < 1. / 3) &&
        (p[1] > -1. / 3))
      return std::sin(this->get_time() * 4 * numbers::PI);
    else
      return 0;
  }
};

template <int dim> class BoundaryValuesV : public Function<dim> {
public:
  virtual double value(const Point<dim> &p,
                       const unsigned int component = 0) const override {
    (void)component;
    Assert(component == 0, ExcIndexRange(component, 0, 1));

    if ((this->get_time() <= 0.5) && (p[0] < 0) && (p[1] < 1. / 3) &&
        (p[1] > -1. / 3))
      return (std::cos(this->get_time() * 4 * numbers::PI) * 4 * numbers::PI);
    else
      return 0;
  }
};

// @sect3{Implementation of the <code>WaveEquation</code> class}

// The implementation of the actual logic is actually fairly short, since we
// relegate things like assembling the matrices and right hand side vectors
// to the library. The rest boils down to not much more than 130 lines of
// actual code, a significant fraction of which is boilerplate code that can
// be taken from previous example programs (e.g. the functions that solve
// linear systems, or that generate output).
//
// Let's start with the constructor (for an explanation of the choice of
// time step, see the section on Courant, Friedrichs, and Lewy in the
// introduction):
template <int dim>
WaveEquation<dim>::WaveEquation()
    : fe(1), dof_handler(Th), time_step(1. / 64), time(time_step),
      timestep_number(1), theta(0.5 + 50 * time_step) {} //

// @sect4{WaveEquation::setup_system}

// The next function is the one that sets up the DoFHandler, and
// matrices and vectors at the beginning of the program, i.e. before the
// first time step. The first few lines are pretty much standard if you've
// read through the tutorial programs at least up to step-6:
template <int dim> void WaveEquation<dim>::setup_system() {

  dof_handler.distribute_dofs(fe);

  std::cout << std::endl
            << "===========================================" << std::endl
            << "Number of active cells: " << triangulation.n_active_cells()
            << std::endl
            << "Number of degrees of freedom: " << dof_handler.n_dofs()
            << std::endl
            << std::endl;

  // add for AMR
  constraints.clear();
  DoFTools::make_hanging_node_constraints(dof_handler, constraints);
  constraints.close();

  DynamicSparsityPattern dsp(dof_handler.n_dofs(), dof_handler.n_dofs());
  DoFTools::make_sparsity_pattern(dof_handler, dsp);
  sparsity_pattern.copy_from(dsp);

  // Then comes a block where we have to initialize the 3 matrices we need
  // in the course of the program: the mass matrix, the Laplace matrix, and
  // the matrix $M+k^2\theta^2A$ used when solving for $U^n$ in each time
  // step.
  //
  // When setting up these matrices, note that they all make use of the same
  // sparsity pattern object. Finally, the reason why matrices and sparsity
  // patterns are separate objects in deal.II (unlike in many other finite
  // element or linear algebra classes) becomes clear: in a significant
  // fraction of applications, one has to hold several matrices that happen
  // to have the same sparsity pattern, and there is no reason for them not
  // to share this information, rather than re-building and wasting memory
  // on it several times.
  //
  // After initializing all of these matrices, we call library functions
  // that build the Laplace and mass matrices. All they need is a DoFHandler
  // object and a quadrature formula object that is to be used for numerical
  // integration. Note that in many respects these functions are better than
  // what we would usually do in application programs, for example because
  // they automatically parallelize building the matrices if multiple
  // processors are available in a machine: for more information see the
  // documentation of WorkStream or the
  // @ref threads "Parallel computing with multiple processors"
  // module. The matrices for solving linear systems will be filled in the
  // run() method because we need to re-apply boundary conditions every time
  // step.
  mass_matrix.reinit(sparsity_pattern);
  laplace_matrix.reinit(sparsity_pattern);
  matrix_u.reinit(sparsity_pattern);
  matrix_v.reinit(sparsity_pattern);

  MatrixCreator::create_mass_matrix(dof_handler, QGauss<dim>(fe.degree + 1),
                                    mass_matrix);
  MatrixCreator::create_laplace_matrix(dof_handler, QGauss<dim>(fe.degree + 1),
                                       laplace_matrix);
  // MatrixCreator::create_mass_matrix(
  //     dof_handler, QGaussSimplex<dim>(fe.degree + 1), mass_matrix);
  // MatrixCreator::create_laplace_matrix(
  //     dof_handler, QGaussSimplex<dim>(fe.degree + 1), laplace_matrix);

  // The rest of the function is spent on setting vector sizes to the
  // correct value. The final line closes the hanging node constraints
  // object. Since we work on a uniformly refined mesh, no constraints exist
  // or have been computed (i.e. there was no need to call
  // DoFTools::make_hanging_node_constraints as in other programs), but we
  // need a constraints object in one place further down below anyway.
  solution_u.reinit(dof_handler.n_dofs());
  solution_v.reinit(dof_handler.n_dofs());
  old_solution_u.reinit(dof_handler.n_dofs());
  old_solution_v.reinit(dof_handler.n_dofs());
  system_rhs.reinit(dof_handler.n_dofs());

  // constraints.close();
}

// @sect4{WaveEquation::solve_u and WaveEquation::solve_v}

// The next two functions deal with solving the linear systems associated
// with the equations for $U^n$ and $V^n$. Both are not particularly
// interesting as they pretty much follow the scheme used in all the
// previous tutorial programs.
//
// One can make little experiments with preconditioners for the two matrices
// we have to invert. As it turns out, however, for the matrices at hand
// here, using Jacobi or SSOR preconditioners reduces the number of
// iterations necessary to solve the linear system slightly, but due to the
// cost of applying the preconditioner it is no win in terms of run-time. It
// is not much of a loss either, but let's keep it simple and just do
// without:
template <int dim> void WaveEquation<dim>::solve_u() {
  SolverControl solver_control(1000, 1e-8 * system_rhs.l2_norm());
  SolverCG<Vector<double>> cg(solver_control);

  cg.solve(matrix_u, solution_u, system_rhs, PreconditionIdentity());

  std::cout << "   u-equation: " << solver_control.last_step()
            << " CG iterations." << std::endl;
}

template <int dim> void WaveEquation<dim>::solve_v() {
  SolverControl solver_control(1000, 1e-8 * system_rhs.l2_norm());
  SolverCG<Vector<double>> cg(solver_control);

  cg.solve(matrix_v, solution_v, system_rhs, PreconditionIdentity());

  std::cout << "   v-equation: " << solver_control.last_step()
            << " CG iterations." << std::endl;
}

// @sect4{WaveEquation::output_results}

// Likewise, the following function is pretty much what we've done
// before. The only thing worth mentioning is how here we generate a string
// representation of the time step number padded with leading zeros to 3
// character length using the Utilities::int_to_string function's second
// argument.
template <int dim> void WaveEquation<dim>::output_results() const {
  DataOut<dim> data_out;

  data_out.attach_dof_handler(dof_handler);
  data_out.add_data_vector(solution_u, "U");
  data_out.add_data_vector(solution_v, "V");

  data_out.build_patches();

  const std::string filename =
      "solution-" + Utilities::int_to_string(timestep_number, 3) + ".vtu";
  // Like step-15, since we write output at every time step (and the system
  // we have to solve is relatively easy), we instruct DataOut to use the
  // zlib compression algorithm that is optimized for speed instead of disk
  // usage since otherwise plotting the output becomes a bottleneck:
  DataOutBase::VtkFlags vtk_flags;
  vtk_flags.compression_level = DataOutBase::CompressionLevel::best_speed;
  data_out.set_flags(vtk_flags);
  std::ofstream output(filename);
  data_out.write_vtu(output);
}

// @sect3.5{<code>WaveEquation::refine_mesh</code>}
//
// This function is the interesting part of the program. It takes care of
// the adaptive mesh refinement. The three tasks
// this function performs is to first find out which cells to
// refine/coarsen, then to actually do the refinement and eventually
// transfer the solution vectors between the two different grids. The first
// task is simply achieved by using the well-established Kelly error
// estimator on the solution. The second task is to actually do the
// remeshing. That involves only basic functions as well, such as the
// <code>refine_and_coarsen_fixed_fraction</code> that refines those cells
// with the largest estimated error that together make up 60 per cent of the
// error, and coarsens those cells with the smallest error that make up for
// a combined 40 per cent of the error. Note that for problems such as the
// current one where the areas where something is going on are shifting
// around, we want to aggressively coarsen so that we can move cells
// around to where it is necessary.
//
// As already discussed in the introduction, too small a mesh leads to
// too small a time step, whereas too large a mesh leads to too little
// resolution. Consequently, after the first two steps, we have two
// loops that limit refinement and coarsening to an allowable range of
// cells:
template <int dim>
void WaveEquation<dim>::refine_mesh(const unsigned int min_grid_level,
                                    const unsigned int max_grid_level) {
  Vector<float> estimated_error_per_cell(Th.n_active_cells());

  std::cout << "* RefineMesh" << std::endl;
  std::cout << "min_grid_level = " << min_grid_level << std::endl;
  std::cout << "max_grid_level = " << max_grid_level << std::endl;
  std::cout << "Th.n_levels()= " << Th.n_levels() << std::endl;

  KellyErrorEstimator<dim>::estimate(
      dof_handler, QGauss<dim - 1>(fe.degree + 1),
      std::map<types::boundary_id, const Function<dim> *>(), solution_u,
      estimated_error_per_cell);

  GridRefinement::refine_and_coarsen_fixed_fraction(
      Th, estimated_error_per_cell, 0.6, 0.4);

  // do not refine mesh that is already at the max refinement level
  if (Th.n_levels() > max_grid_level)
    for (const auto &cell : Th.active_cell_iterators_on_level(max_grid_level))
      cell->clear_refine_flag();
  // do not coarsen mesh that is already at the min refinement level
  for (const auto &cell : Th.active_cell_iterators_on_level(min_grid_level))
    cell->clear_coarsen_flag();
  // These two loops above are slightly different but this is easily
  // explained. In the first loop, instead of calling
  // <code>triangulation.end()</code> we may as well have called
  // <code>triangulation.end_active(max_grid_level)</code>. The two
  // calls should yield the same iterator since iterators are sorted
  // by level and there should not be any cells on levels higher than
  // on level <code>max_grid_level</code>. In fact, this very piece
  // of code makes sure that this is the case.

  // As part of mesh refinement we need to transfer the solution vectors
  // from the old mesh to the new one. To this end we use the
  // SolutionTransfer class and we have to prepare the solution vectors that
  // should be transferred to the new grid (we will lose the old grid once
  // we have done the refinement so the transfer has to happen concurrently
  // with refinement). At the point where we call this function, we will
  // have just computed the solution, so we no longer need the old_solution
  // variable (it will be overwritten by the solution just after the mesh
  // may have been refined, i.e., at the end of the time step; see below).
  // In other words, we only need the one solution vector, and we copy it
  // to a temporary object where it is safe from being reset when we further
  // down below call <code>setup_system()</code>.
  //
  // Consequently, we initialize a SolutionTransfer object by attaching
  // it to the old DoF handler. We then prepare the triangulation and the
  // data vector for refinement (in this order).
  SolutionTransfer<dim> solution_transfer(dof_handler);

  Th.prepare_coarsening_and_refinement();

  Vector<double> previous_solution_u;
  previous_solution_u = solution_u;
  Vector<double> previous_solution_v;
  previous_solution_v = solution_v;
  std::vector<Vector<double>> all_in{previous_solution_u, previous_solution_v};

  std::cout << "all_in[0].size()=" << all_in[0].size() << std::endl;
  std::cout << "all_in[1].size()=" << all_in[1].size() << std::endl;
  std::cout << "dof_handler->n_dofs()=" << dof_handler.n_dofs() << std::endl;

  solution_transfer.prepare_for_coarsening_and_refinement(all_in);

  // Now everything is ready, so do the refinement and recreate the DoF
  // structure on the new grid, and finally initialize the matrix structures
  // and the new vectors in the <code>setup_system</code> function. Next, we
  // actually perform the interpolation of the solution from old to new
  // grid. The final step is to apply the hanging node constraints to the
  // solution vector, i.e., to make sure that the values of degrees of
  // freedom located on hanging nodes are so that the solution is
  // continuous. This is necessary since SolutionTransfer only operates on
  // cells locally, without regard to the neighborhood.
  Th.execute_coarsening_and_refinement();
  setup_system();

  std::vector<Vector<double>> all_out(2);
  all_out[0].reinit(solution_u);
  all_out[1].reinit(solution_v);

  solution_transfer.interpolate(all_in, all_out);

  solution_u = all_out[0];
  solution_v = all_out[1];

  constraints.distribute(solution_u);
  constraints.distribute(solution_v);
}

// @sect4{WaveEquation::run}

// The following is really the only interesting function of the program. It
// contains the loop over all time steps, but before we get to that we have
// to set up the grid, DoFHandler, and matrices. In addition, we have to
// somehow get started with initial values. To this end, we use the
// VectorTools::project function that takes an object that describes a
// continuous function and computes the $L^2$ projection of this function
// onto the finite element space described by the DoFHandler object. Can't
// be any simpler than that:
template <int dim> void WaveEquation<dim>::run() {
  const unsigned int initial_global_refinement = 4;       // 2;
  const unsigned int n_adaptive_pre_refinement_steps = 4; // 4;
  GridGenerator::hyper_cube(triangulation, -1, 1);
  // GridGenerator::convert_hypercube_to_simplex_mesh(triangulation, Th);
  Th.copy_triangulation(triangulation);
  Th.refine_global(initial_global_refinement);
  std::cout << "Th.n_levels()=" << Th.n_levels() << std::endl;
  std::cout << "triangulation.n_levels()=" << triangulation.n_levels()
            << std::endl;

  std::cout << "Number of active cells: " << Th.n_active_cells() << std::endl;

  setup_system();

  unsigned int pre_refinement_step = 0;

  // The next thing is to loop over all the time steps until we reach the
  // end time ($T=5$ in this case). In each time step, we first have to
  // solve for $U^n$, using the equation $(M^n + k^2\theta^2 A^n)U^n =$
  // $(M^{n,n-1} - k^2\theta(1-\theta) A^{n,n-1})U^{n-1} + kM^{n,n-1}V^{n-1}
  // +$ $k\theta \left[k \theta F^n + k(1-\theta) F^{n-1} \right]$. Note
  // that we use the same mesh for all time steps, so that $M^n=M^{n,n-1}=M$
  // and $A^n=A^{n,n-1}=A$. What we therefore have to do first is to add up
  // $MU^{n-1} - k^2\theta(1-\theta) AU^{n-1} + kMV^{n-1}$ and the forcing
  // terms, and put the result into the <code>system_rhs</code> vector. (For
  // these additions, we need a temporary vector that we declare before the
  // loop to avoid repeated memory allocations in each time step.)
  //
  // The one thing to realize here is how we communicate the time variable
  // to the object describing the right hand side: each object derived from
  // the Function class has a time field that can be set using the
  // Function::set_time and read by Function::get_time. In essence, using
  // this mechanism, all functions of space and time are therefore
  // considered functions of space evaluated at a particular time. This
  // matches well what we typically need in finite element programs, where
  // we almost always work on a single time step at a time, and where it
  // never happens that, for example, one would like to evaluate a
  // space-time function for all times at any given spatial location.
  Vector<double> tmp;
  Vector<double> forcing_terms;

start_time_iteration:

  time = 0.0;
  timestep_number = 0;

  tmp.reinit(solution_u.size());
  forcing_terms.reinit(solution_u.size());

  // VectorTools::project(dof_handler, constraints,
  //                      QGaussSimplex<dim>(fe.degree + 1),
  //                      InitialValuesU<dim>(), old_solution_u);
  // VectorTools::project(dof_handler, constraints,
  //                      QGaussSimplex<dim>(fe.degree + 1),
  //                      InitialValuesV<dim>(), old_solution_v);

  VectorTools::interpolate(dof_handler, Functions::ZeroFunction<dim>(),
                           old_solution_u);
  VectorTools::interpolate(dof_handler, Functions::ZeroFunction<dim>(),
                           old_solution_v);
  solution_u = old_solution_u;
  solution_v = old_solution_v;

  output_results();

  while (time <= 5) {
    time += time_step;
    ++timestep_number;
    std::cout << "Time step " << timestep_number << " at t=" << time
              << std::endl;

    mass_matrix.vmult(system_rhs, old_solution_u);

    mass_matrix.vmult(tmp, old_solution_v);
    system_rhs.add(time_step, tmp);

    laplace_matrix.vmult(tmp, old_solution_u);
    system_rhs.add(-theta * (1 - theta) * time_step * time_step, tmp);

    RightHandSide<dim> rhs_function;
    rhs_function.set_time(time);
    VectorTools::create_right_hand_side(dof_handler, QGauss<dim>(fe.degree + 1),
                                        rhs_function, tmp);
    // VectorTools::create_right_hand_side(
    //     dof_handler, QGaussSimplex<dim>(fe.degree + 1), rhs_function, tmp);
    forcing_terms = tmp;
    forcing_terms *= theta * time_step;

    rhs_function.set_time(time - time_step);
    VectorTools::create_right_hand_side(dof_handler, QGauss<dim>(fe.degree + 1),
                                        rhs_function, tmp);
    // VectorTools::create_right_hand_side(
    //     dof_handler, QGaussSimplex<dim>(fe.degree + 1), rhs_function, tmp);

    forcing_terms.add((1 - theta) * time_step, tmp);

    system_rhs.add(theta * time_step, forcing_terms);

    // After so constructing the right hand side vector of the first
    // equation, all we have to do is apply the correct boundary
    // values. As for the right hand side, this is a space-time function
    // evaluated at a particular time, which we interpolate at boundary
    // nodes and then use the result to apply boundary values as we
    // usually do. The result is then handed off to the solve_u()
    // function:
    {
      BoundaryValuesU<dim> boundary_values_u_function;
      boundary_values_u_function.set_time(time);

      std::map<types::global_dof_index, double> boundary_values;
      VectorTools::interpolate_boundary_values(
          dof_handler, 0, boundary_values_u_function, boundary_values);

      // The matrix for solve_u() is the same in every time steps, so one
      // could think that it is enough to do this only once at the
      // beginning of the simulation. However, since we need to apply
      // boundary values to the linear system (which eliminate some matrix
      // rows and columns and give contributions to the right hand side),
      // we have to refill the matrix in every time steps before we
      // actually apply boundary data. The actual content is very simple:
      // it is the sum of the mass matrix and a weighted Laplace matrix:
      matrix_u.copy_from(mass_matrix);
      matrix_u.add(theta * theta * time_step * time_step, laplace_matrix);
      MatrixTools::apply_boundary_values(boundary_values, matrix_u, solution_u,
                                         system_rhs);
    }
    solve_u();

    // The second step, i.e. solving for $V^n$, works similarly, except
    // that this time the matrix on the left is the mass matrix (which we
    // copy again in order to be able to apply boundary conditions, and
    // the right hand side is $MV^{n-1} - k\left[ \theta A U^n +
    // (1-\theta) AU^{n-1}\right]$ plus forcing terms. Boundary values
    // are applied in the same way as before, except that now we have to
    // use the BoundaryValuesV class:
    laplace_matrix.vmult(system_rhs, solution_u);
    system_rhs *= -theta * time_step;

    mass_matrix.vmult(tmp, old_solution_v);
    system_rhs += tmp;

    laplace_matrix.vmult(tmp, old_solution_u);
    system_rhs.add(-time_step * (1 - theta), tmp);

    system_rhs += forcing_terms;

    {
      BoundaryValuesV<dim> boundary_values_v_function;
      boundary_values_v_function.set_time(time);

      std::map<types::global_dof_index, double> boundary_values;
      VectorTools::interpolate_boundary_values(
          dof_handler, 0, boundary_values_v_function, boundary_values);
      matrix_v.copy_from(mass_matrix);
      MatrixTools::apply_boundary_values(boundary_values, matrix_v, solution_v,
                                         system_rhs);
    }
    solve_v();

    // Finally, after both solution components have been computed, we
    // output the result, compute the energy in the solution, and go on to
    // the next time step after shifting the present solution into the
    // vectors that hold the solution at the previous time step. Note the
    // function SparseMatrix::matrix_norm_square that can compute
    // $\left<V^n,MV^n\right>$ and $\left<U^n,AU^n\right>$ in one step,
    // saving us the expense of a temporary vector and several lines of
    // code:
    output_results();

    std::cout << "   Total energy: "
              << (mass_matrix.matrix_norm_square(solution_v) +
                  laplace_matrix.matrix_norm_square(solution_u)) /
                     2
              << std::endl;

    // ...take care of mesh refinement. Here, what we want to do is
    // (i) refine the requested number of times at the very beginning
    // of the solution procedure, after which we jump to the top to
    // restart the time iteration, (ii) refine every fifth time
    // step after that.
    //
    // The time loop and, indeed, the main part of the program ends
    // with starting into the next time step by setting old_solution
    // to the solution we have just computed.
    if ((timestep_number == 1) &&
        (pre_refinement_step < n_adaptive_pre_refinement_steps)) {
      refine_mesh(initial_global_refinement,
                  initial_global_refinement + n_adaptive_pre_refinement_steps);
      ++pre_refinement_step;

      tmp.reinit(solution_u.size());
      forcing_terms.reinit(solution_u.size());

      std::cout << std::endl;

      std::cout << "timestep_number = " << timestep_number << std::endl;
      std::cout << "pre_refinement_step= " << pre_refinement_step << std::endl;

      goto start_time_iteration;
    } else if ((timestep_number > 0) && (timestep_number % 5 == 0)) {
      refine_mesh(initial_global_refinement,
                  initial_global_refinement + n_adaptive_pre_refinement_steps);
      tmp.reinit(solution_u.size());
      forcing_terms.reinit(solution_u.size());
    }

    old_solution_u = solution_u;
    old_solution_v = solution_v;
  }
}
} // namespace Step23

// @sect3{The <code>main</code> function}

// What remains is the main function of the program. There is nothing here
// that hasn't been shown in several of the previous programs:
int main() {

  using std::chrono::duration;
  using std::chrono::duration_cast;
  using std::chrono::high_resolution_clock;
  using std::chrono::milliseconds;

  auto t1 = high_resolution_clock::now();
  try {
    using namespace Step23;

    WaveEquation<2> wave_equation_solver;
    wave_equation_solver.run();
  } catch (std::exception &exc) {
    std::cerr << std::endl
              << std::endl
              << "----------------------------------------------------"
              << std::endl;
    std::cerr << "Exception on processing: " << std::endl
              << exc.what() << std::endl
              << "Aborting!" << std::endl
              << "----------------------------------------------------"
              << std::endl;

    return 1;
  } catch (...) {
    std::cerr << std::endl
              << std::endl
              << "----------------------------------------------------"
              << std::endl;
    std::cerr << "Unknown exception!" << std::endl
              << "Aborting!" << std::endl
              << "----------------------------------------------------"
              << std::endl;
    return 1;
  }

  auto t2 = high_resolution_clock::now();

  /* Getting number of milliseconds as an integer. */
  auto ms_int = duration_cast<milliseconds>(t2 - t1);

  /* Getting number of milliseconds as a double. */
  duration<double, std::milli> ms_double = t2 - t1;

  std::cout << ms_int.count() << "ms\n";
  std::cout << ms_double.count() << "ms\n";

  return 0;
}
