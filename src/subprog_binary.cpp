/**
 ==============================================================================
 Copyright 2019, Jonathan Zrake

 Permission is hereby granted, free of charge, to any person obtaining a copy of
 this software and associated documentation files (the "Software"), to deal in
 the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.

 ==============================================================================
*/
#include "app_compile_opts.hpp"
#if MARA_COMPILE_SUBPROGRAM_BINARY




#include <iostream>
#include "subprog_binary.hpp"
#include "mesh_tree_operators.hpp"
#include "core_ndarray_ops.hpp"
#include "app_serialize.hpp"
#include "app_parallel.hpp"
#include "app_serialize_tree.hpp"
#include "app_subprogram.hpp"
#include "app_filesystem.hpp"




//=============================================================================
namespace binary
{
    auto next_solution(const solution_t& solution, const solver_data_t& solver_data);
    auto next_schedule(const state_t& state);
    auto next_state(const state_t& state, const solver_data_t& solver_data);
    auto run_tasks(const state_t& state, const solver_data_t& solver_data);
    auto simulation_should_continue(const state_t& state);
}




//=============================================================================
mara::config_template_t binary::create_config_template()
{
    return mara::make_config_template()
    .item("restart",             std::string())
    .item("outdir",              "data")        // directory where data products are written to
    .item("cpi",                 10.0)          // checkpoint interval (orbits; chkpt.????.h5 - snapshot of app_state)
    .item("dfi",                  1.0)          // diagnostic field interval (orbits; diagnostics.????.h5)
    .item("tsi",                 2e-3)          // time series interval (orbits)
    .item("tfinal",               1.0)          // simulation stop time (orbits)
    .item("cfl_number",           0.4)          // the Courant number to use
    .item("depth",                  4)
    .item("conserve_linear_p",      0)          // set to true to use linear momentum conserving variables
    .item("block_size",            24)
    .item("focus_factor",        2.00)
    .item("focus_index",         2.00)
    .item("threaded",               1)          // set to 0 to disable multi-threaded tree updates
    .item("rk_order",               2)          // time-stepping Runge-Kutta order: 1 or 2
    .item("reconstruct_method", "plm")          // zone extrapolation method: pcm or plm
    .item("plm_theta",            1.8)          // plm theta parameter: [1.0, 2.0]
    .item("riemann",           "hlle")          // riemann solver to use: hlle only (hllc disabled until further testing)
    .item("softening_radius",    0.05)          // gravitational softening radius
    .item("source_term_softening", 1.)          // number of cells within which the Sr source term is suppressed
    .item("sink_radius",         0.05)          // radius of mass (and momentum) subtraction region
    .item("sink_rate",           50.0)          // sink rate at the point masses (orbital angular frequency)
    .item("buffer_damping_rate", 10.0)          // maximum rate of buffer zone, where solution is driven to initial state
    .item("domain_radius",       24.0)          // half-size of square domain
    .item("disk_radius",          2.0)          // characteristic disk radius (in units of binary separation)
    .item("disk_mass",           1e-3)          // total disk mass (in units of the binary mass)
    .item("ambient_density",     1e-4)          // surface density beyond torus (relative to mas sigma)
    .item("separation",           1.0)          // binary separation: 0.0 or 1.0 (zero emulates a single body)
    .item("mass_ratio",           1.0)          // binary mass ratio M2 / M1: (0.0, 1.0]
    .item("eccentricity",         0.0)          // orbital eccentricity: [0.0, 1.0)
    .item("counter_rotate",         0)          // retrograde disk option: 0 or 1
    .item("mach_number",         40.0)          // disk mach number; for locally isothermal EOS
    .item("axisymmetric_cs2",       1)          // if true then cs2 = GM / r / Mach^2; otherwise cs2 = -phi / Mach^2
    .item("alpha_cutoff_radius",  0.0)          // radius inside of which viscosity is set to zero
    .item("alpha",                0.0)          // viscous alpha coefficient (if nu == 0 then alpha-viscosity is used)
    .item("nu",                   0.0);         // kinematic viscosity coefficient (if nu > 0 then constant-nu is used)
}




//=============================================================================
binary::primitive_field_t binary::create_disk_profile(const mara::config_t& run_config)
{
    auto softening_radius  = run_config.get_double("softening_radius");
    auto disk_radius       = run_config.get_double("disk_radius");
    auto mach_number       = run_config.get_double("mach_number");
    auto disk_mass         = run_config.get_double("disk_mass");
    auto ambient_density   = run_config.get_double("ambient_density");
    auto counter_rotate    = run_config.get_int("counter_rotate");
    auto rc = disk_radius;
    auto s0 = disk_mass / (17.0618 * rc * rc); // see mathematica notebook
    auto s1 = ambient_density * s0;

    auto sigma = [=] (double r)
    {
        auto x = r / rc;
        return s0 * std::exp(-0.5 * (x - 1) * (x - 1)) + s1;
    };

    auto dp_dr = [=] (double r)
    {
        auto GM = 1.0;
        auto Ma = mach_number;
        auto rs = softening_radius;
        auto x = r / rc;
        return (GM / Ma / Ma / (r + rs)) * (x * (1 - x) * (1 - s1 / sigma(r)) - 1.0);
    };

    return [=] (location_2d_t point)
    {
        auto x              = point[0].value;
        auto y              = point[1].value;
        auto r2             = x * x + y * y;
        auto r              = std::sqrt(r2);
        auto rs             = softening_radius;
        auto GM             = 1.0;
        auto vp             = std::sqrt(GM / (r + rs) + dp_dr(r)) * (counter_rotate ? -1 : 1);
        auto vx             = vp * (-y / r);
        auto vy             = vp * ( x / r);

        return mara::iso2d::primitive_t()
            .with_sigma(sigma(r))
            .with_velocity_x(vx)
            .with_velocity_y(vy);
    };
}

mara::config_t binary::create_run_config(int argc, const char* argv[])
{
    auto args = mara::argv_to_string_map(argc, argv);
    return args.count("restart")
    ? create_config_template()
            .create()
            .update(mara::read_config(h5::File(args.at("restart"), "r").open_group("run_config")))
            .update(args)
    : create_config_template().create().update(args);
}

binary::quad_tree_t<binary::location_2d_t> binary::create_vertices(const mara::config_t& run_config)
{
    auto domain_radius = run_config.get_double("domain_radius");
    auto focus_factor  = run_config.get_double("focus_factor");
    auto focus_index   = run_config.get_double("focus_index");
    auto block_size    = run_config.get_int("block_size");
    auto depth         = run_config.get_int("depth");

    // 1. define predicate for building topology of the grid
    // 2. call tree_with_topology<2>(predicate) to get tree of indexes
    // 3. from topology build rank_tree
    // 4. function that maps tree indexes to vertex block (block_size, domain_radius)

    auto centroid_radius = [] (mara::tree_index_t<2> index)
    {
        double x = 2.0 * ((index.coordinates[0] + 0.5) / (1 << index.level) - 0.5);
        double y = 2.0 * ((index.coordinates[1] + 0.5) / (1 << index.level) - 0.5);
        return std::sqrt(x * x + y * y);
    };

    auto refinement_radius = [focus_factor, focus_index] (std::size_t level, double centroid_radius)
    {
        return centroid_radius < focus_factor / std::pow(level, focus_index);
    };

    auto topology = mara::tree_with_topology<2>([refinement_radius, centroid_radius, depth] (auto index) 
    { 
        return refinement_radius(index.level, centroid_radius(index)) && index.level < depth; 
    });
    // auto topology = mara::tree_with_topology<2>([] (auto index)
    // {
    //     return index.level < 3;
    // });


    //=========================================================================
    auto my_rank   = mpi::comm_world().rank();
    auto rank_tree = mara::build_rank_tree<2>(
        mara::ensure_valid_quadtree(topology, [] (auto i) { return i.child_indexes(); }), 
        mpi::comm_world().size());

    auto build_my_vertex_blocks = [my_rank, domain_radius, block_size] (auto ir)
    {
        if (ir.second == my_rank)
        {
            auto index = ir.first;
            auto block_length = domain_radius / (1 << (index.level - 1));

            // auto x0 = index.coordinates[0] * block_length;
            // auto y0 = index.coordinates[1] * block_length;
            auto x0 = index.coordinates[0] * block_length - domain_radius;
            auto y0 = index.coordinates[1] * block_length - domain_radius;
            auto x_points = nd::linspace(0, 1, block_size + 1) * block_length + x0;
            auto y_points = nd::linspace(0, 1, block_size + 1) * block_length + y0;

            return nd::cartesian_product(x_points, y_points)
                | nd::apply([] (double x, double y) { return binary::location_2d_t{x, y}; })
                | nd::to_shared();
        }
        return nd::shared_array<binary::location_2d_t, 2>{};
    };

    return rank_tree.pair_indexes().map(build_my_vertex_blocks);  
}

mara::orbital_elements_t binary::create_binary_params(const mara::config_t& run_config)
{
    auto binary = mara::orbital_elements_t();
    binary.total_mass   = 1.0;
    binary.separation   = run_config.get_double("separation");
    binary.mass_ratio   = run_config.get_double("mass_ratio");
    binary.eccentricity = run_config.get_double("eccentricity");
    return binary;
}

binary::solution_t binary::create_solution(const mara::config_t& run_config)
{
    auto conserved_u = create_vertices(run_config).map([&run_config] (auto block)
    {
        if (block.size() == 0)
        {
            return block | nd::map(create_disk_profile(run_config)) 
            | nd::map([] (auto p) { return p.to_conserved_per_area(); }) 
            | nd::to_shared();
        }
        
        auto cell_centers = block | nd::midpoint_on_axis(0) | nd::midpoint_on_axis(1);
        auto primitive = cell_centers | nd::map(create_disk_profile(run_config));
        return primitive
        | nd::map([] (auto p) { return p.to_conserved_per_area(); })
        | nd::to_shared();
    });

    auto conserved_q = create_vertices(run_config).map([&run_config] (auto block)
    {
        if (block.size() == 0)
        {
            auto prim = block | nd::map(create_disk_profile(run_config));
            return nd::zip(block, prim) 
            | nd::apply([] (auto x, auto p) { return p.to_conserved_angmom_per_area(x); })
            | nd::to_shared();
        }

        auto cell_centers = block | nd::midpoint_on_axis(0) | nd::midpoint_on_axis(1);
        auto primitive = cell_centers | nd::map(create_disk_profile(run_config));
        return nd::zip(cell_centers, primitive)
        | nd::apply([] (auto x, auto p) { return p.to_conserved_angmom_per_area(x); })
        | nd::to_shared();
    });

    auto conserve_linear_p = run_config.get_int("conserve_linear_p");

    return solution_t{
        0, 0.0,
        conserve_linear_p ? conserved_u : decltype(conserved_u){},
        conserve_linear_p ? decltype(conserved_q){} : conserved_q,
        {}, {}, {}, {}, {}, {},
        mara::make_full_orbital_elements_with_zeros(),
        mara::make_full_orbital_elements_with_zeros(),
    };
}

mara::schedule_t binary::create_schedule(const mara::config_t& run_config)
{
    auto schedule = mara::schedule_t();
    schedule.create_and_mark_as_due("write_checkpoint");
    schedule.create_and_mark_as_due("write_diagnostics");
    schedule.create_and_mark_as_due("record_time_series");
    return schedule;
}

binary::state_t binary::create_state(const mara::config_t& run_config)
{
    auto restart = run_config.get<std::string>("restart");

    if (restart.empty())
    {
        return {
            create_solution(run_config),
            create_schedule(run_config),
            {},
            run_config,
        };
    }
    return mara::read<state_t>(h5::File(restart, "r").open_group("/"), "/").with(run_config);
}



//=============================================================================
auto binary::next_solution(const solution_t& solution, const solver_data_t& solver_data)
{
    auto can_fail = [] (const solution_t& solution, const solver_data_t& solver_data, auto dt, bool safe_mode)
    {
        auto s0 = solution;

        switch (solver_data.rk_order)
        {
            case 1:
            {
                return advance(s0, solver_data, dt, safe_mode);
            }
            case 2:
            {
                auto b0 = mara::make_rational(1, 2);
                auto s1 = advance(s0, solver_data, dt, safe_mode);
                auto s2 = advance(s1, solver_data, dt, safe_mode);
                return s0 * b0 + s2 * (1 - b0);
            }
        }
        throw std::invalid_argument("binary::next_solution");
    };

    return can_fail(solution, solver_data, solver_data.recommended_time_step, false);

    //Going to break the functional rules to make this work in parallel for now...
    // bool safe = false;
    // solution_t next_solution;
    // try {
    //     next_solution = can_fail(solution, solver_data, solver_data.recommended_time_step, false);
    // }
    // catch (const std::exception& e)
    // {
    //     std::cout << e.what() << std::endl;
    //     safe = true;
    //     // return can_fail(solution, solver_data, solver_data.recommended_time_step * 0.5, true);
    // }

    // bool use_safe_mode = mpi::comm_world().all_reduce(safe, mpi::operation::lor);
    // if (use_safe_mode)
    // {
    //     mpi::printf_master("%s\n", "safe mode");
    //     return can_fail(solution, solver_data, solver_data.recommended_time_step * 0.5, true); 
    // }
    // return next_solution;
}

auto binary::next_schedule(const state_t& state)
{
    return mara::mark_tasks_in(state, state.solution.time.value,
        {{"write_checkpoint",   state.run_config.get_double("cpi") * 2 * M_PI},
         {"write_diagnostics",  state.run_config.get_double("dfi") * 2 * M_PI},
         {"record_time_series", state.run_config.get_double("tsi") * 2 * M_PI}});
}

auto binary::next_state(const state_t& state, const solver_data_t& solver_data)
{
    return state_t{
        next_solution(state.solution, solver_data),
        next_schedule(state),
        state.time_series,
        state.run_config,
    };
}




//=============================================================================
auto binary::simulation_should_continue(const state_t& state)
{
    return state.solution.time / (2 * M_PI) < state.run_config.get_double("tfinal");
}




//=============================================================================
auto binary::run_tasks(const state_t& state, const solver_data_t& solver_data)
{
    auto comm        = mpi::comm_world();
    auto my_rank     = comm.rank();
    auto rank_tree   = solver_data.domain_decomposition;
    auto is_my_block = [rank_tree, my_rank] (auto idx)
    { 
        if (! rank_tree.node_at(idx).has_value())
            return false;

        return rank_tree.at(idx) == my_rank; 
    };


    //=========================================================================
    auto write_checkpoint  = [rank_tree, is_my_block, &comm] (const state_t& state)
    {
        auto outdir = state.run_config.get_string("outdir");
        auto count  = state.schedule.num_times_performed("write_checkpoint");
        auto fname  = mara::filesystem::join(outdir, mara::create_numbered_filename("chkpt", count, "h5"));
        auto next_state = mara::complete_task_in(state, "write_checkpoint");

        mpi::printf_master("write checkpoint: %s\n", fname.data());
        if (mpi::is_master())
        {
            auto group = h5::File(fname, "w").open_group("/");
            binary::write_singles(group, "/", next_state);
        }
        comm.barrier();

        for(auto rank : nd::arange(comm.size()))
        {
            comm.barrier();
            if (rank != comm.rank())
                continue;

            auto group = h5::File(fname, "r+").open_group("/");
            binary::write_parallels(group, "/", next_state, is_my_block);
        }
        return next_state;
    };


    //=========================================================================
    auto write_diagnostics = [rank_tree, is_my_block, &comm] (const state_t& state)
    {
        auto outdir = state.run_config.get_string("outdir");
        auto count  = state.schedule.num_times_performed("write_diagnostics");
        auto fname  = mara::filesystem::join(outdir, mara::create_numbered_filename("diagnostics", count, "h5"));
        auto diagnostic = diagnostic_fields(state.solution, state.run_config);

        mpi::printf_master("write diagnostics: %s\n", fname.data());
        if (mpi::is_master())
        {
            auto group = h5::File(fname, "w").open_group("/");
            binary::write_singles(group, "/", diagnostic);
        }
        comm.barrier();

        for(auto rank : nd::arange(comm.size()))
        {
            comm.barrier();
            if (rank != comm.rank())
                continue;

            auto group = h5::File(fname, "r+").open_group("/");
            binary::write_parallels(group, "/", diagnostic, is_my_block);
        }
        return mara::complete_task_in(state, "write_diagnostics");
    };


    //=========================================================================
    auto record_time_series = [&solver_data] (state_t state)
    {
        auto sample = time_series_sample_t();
        sample.time                         = state.solution.time;
        sample.mass_accreted_on             = state.solution.mass_accreted_on;
        sample.angular_momentum_accreted_on = state.solution.angular_momentum_accreted_on;
        sample.integrated_torque_on         = state.solution.integrated_torque_on;
        sample.work_done_on                 = state.solution.work_done_on;
        sample.mass_ejected                 = state.solution.mass_ejected;
        sample.angular_momentum_ejected     = state.solution.angular_momentum_ejected;
        sample.disk_mass                    = disk_mass            (state.solution, solver_data);
        sample.disk_angular_momentum        = disk_angular_momentum(state.solution, solver_data);
        sample.orbital_elements_acc         = state.solution.orbital_elements_acc;
        sample.orbital_elements_grav        = state.solution.orbital_elements_grav;

        state.time_series = state.time_series.prepend(sample);
        return mara::complete_task_in(state, "record_time_series");
    };


    return mara::run_scheduled_tasks(state, {
        {"write_diagnostics", write_diagnostics},
        {"record_time_series", record_time_series},
        {"write_checkpoint",  write_checkpoint}});
}

void binary::prepare_filesystem(const mara::config_t& run_config)
{
    auto outdir = run_config.get_string("outdir");
    mara::filesystem::require_dir(outdir);
}

void binary::print_run_loop_message(const state_t& state, const solver_data_t& solver_data, mara::perf_diagnostics_t perf)
{
    // how calc this in parallel?
    auto kzps = solver_data.cell_centers
    .map([] (auto&& block) { return block.size(); })
    .sum() / perf.execution_time_ms;

    mpi::printf_master("[%04d] orbits=%3.7lf kzps=%3.2lf\n",
        state.solution.iteration.as_integral(),
        state.solution.time.value / (2 * M_PI), kzps);
    std::fflush(stdout);
}




//=============================================================================
class subprog_binary : public mara::sub_program_t
{
public:

    int main(int argc, const char* argv[]) override
    {
        auto session     = mpi::Session();
        auto run_config  = binary::create_run_config(argc, argv);
        auto solver_data = binary::create_solver_data(run_config);
        auto state       = binary::create_state(run_config);
        auto next        = std::bind(binary::next_state, std::placeholders::_1, solver_data);
        auto tasks       = std::bind(binary::run_tasks, std::placeholders::_1, solver_data);
        auto perf        = mara::perf_diagnostics_t();

        binary::prepare_filesystem(run_config);
        binary::set_scheme_globals(run_config);
        
        if (mpi::is_master())
        {
            mara::pretty_print(std::cout, "config", run_config);
        }
        
        state = tasks(state);
        while (binary::simulation_should_continue(state))
        {
            std::tie(state, perf) = mara::time_execution(mara::compose(tasks, next), state);
            binary::print_run_loop_message(state, solver_data, perf);
        }

        tasks(next(state));
        return 0;
    }

    std::string name() const override
    {
        return "binary";
    }
};

std::unique_ptr<mara::sub_program_t> make_subprog_binary()
{
    return std::make_unique<subprog_binary>();
}

#endif // MARA_COMPILE_SUBPROGRAM_BINARY
