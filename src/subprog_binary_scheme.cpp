#include "core_ndarray_ops.hpp"
#include "core_thread_pool.hpp"
#include "math_interpolation.hpp"
#include "mesh_prolong_restrict.hpp"
#include "mesh_tree_operators.hpp"
#include "subprog_binary.hpp"
#if MARA_COMPILE_SUBPROGRAM_BINARY


#include "app_parallel.hpp"
#include "app_loads_dumps.hpp"


static mara::thread_pool_t tree_launch(1);
using prim_pair_t = std::tuple<mara::iso2d::primitive_t, mara::iso2d::primitive_t>;
using force_per_area_t = mara::arithmetic_sequence_t<mara::dimensional_value_t<-1, 1, -2, double>, 2>;




//=============================================================================
namespace binary
{
    struct source_term_total_t
    {
        mara::arithmetic_sequence_t<mara::unit_mass    <double>, 2> mass_accreted_on = {};
        mara::arithmetic_sequence_t<mara::unit_angmom  <double>, 2> angular_momentum_accreted_on = {};
        mara::arithmetic_sequence_t<mara::unit_angmom  <double>, 2> integrated_torque_on = {};
        mara::arithmetic_sequence_t<mara::unit_momentum<double>, 2> momentum_x_accreted_on = {};
        mara::arithmetic_sequence_t<mara::unit_momentum<double>, 2> momentum_y_accreted_on = {};
        mara::arithmetic_sequence_t<mara::unit_momentum<double>, 2> integrated_force_x_on = {};
        mara::arithmetic_sequence_t<mara::unit_momentum<double>, 2> integrated_force_y_on = {};
        mara::arithmetic_sequence_t<mara::unit_energy  <double>, 2> work_done_on = {};
        mara::unit_mass  <double>                                 mass_ejected = {};
        mara::unit_angmom<double>                                 angular_momentum_ejected = {};
        source_term_total_t operator+(const source_term_total_t& other) const;
    };
}




//=============================================================================
void binary::set_scheme_globals(const mara::config_t& run_config)
{
    if (run_config.get_int("threaded") <= 0)
    {
        throw std::invalid_argument("runtime option 'threaded' for number of threads must be > 0");
    }
    tree_launch.restart(run_config.get_int("threaded"));
}




//=============================================================================
template<std::size_t I>
static auto component()
{
    return nd::map([] (auto p) { return mara::get<I>(p); });
};




//=============================================================================
binary::source_term_total_t binary::source_term_total_t::operator+(const source_term_total_t& other) const
{
    return {
        mass_accreted_on + other.mass_accreted_on,
        angular_momentum_accreted_on + other.angular_momentum_accreted_on,
        integrated_torque_on + other.integrated_torque_on,
        momentum_x_accreted_on + other.momentum_x_accreted_on,
        momentum_y_accreted_on + other.momentum_y_accreted_on,
        integrated_force_x_on + other.integrated_force_x_on,
        integrated_force_y_on + other.integrated_force_y_on,
        work_done_on + other.work_done_on,
        mass_ejected + other.mass_ejected,
        angular_momentum_ejected + other.angular_momentum_ejected,
    };
}




//=============================================================================
static auto grav_vdot_field(const binary::solver_data_t& solver_data, binary::location_2d_t body_location, mara::unit_mass<double> body_mass)
{
    return [body_location, body_mass, softening_radius=solver_data.softening_radius](binary::location_2d_t field_point)
    {
        auto G   = mara::dimensional_value_t<3, -1, -2, double>(1.0);
        auto dr  = field_point - body_location;
        auto dr2 = dr[0] * dr[0] + dr[1] * dr[1];
        auto rs2 = softening_radius * softening_radius;
        return -dr / (dr2 + rs2).pow<3, 2>() * G * body_mass;
    };
}




//=============================================================================
static auto grav_phi_field(const binary::solver_data_t& solver_data, binary::location_2d_t body_location, mara::unit_mass<double> body_mass)
{
    return [body_location, body_mass, softening_radius=solver_data.softening_radius](binary::location_2d_t field_point)
    {
        auto G   = mara::dimensional_value_t<3, -1, -2, double>(1.0);
        auto dr  = field_point - body_location;
        auto dr2 = dr[0] * dr[0] + dr[1] * dr[1];
        auto rs2 = softening_radius * softening_radius;
        return -G * body_mass / (dr2 + rs2).pow<1, 2>();
    };
}




//=============================================================================
static auto sink_rate_field(const binary::solver_data_t& solver_data, binary::location_2d_t sink_location)
{
    return [sink_location, sink_radius=solver_data.sink_radius, sink_rate=solver_data.sink_rate] (binary::location_2d_t field_point)
    {
        auto dr = field_point - sink_location;
        auto s2 = sink_radius * sink_radius;
        auto a2 = (dr * dr).sum() / s2 / 2.0;
        return sink_rate * 0.5 * std::exp(-a2);
    };
}




//=============================================================================
template<typename TreeType>
static auto extend(TreeType tree, std::size_t axis, std::size_t guard_count)
{
    return tree.indexes().map([tree, axis, guard_count] (auto index)
    {
        auto C = tree.at(index);
        auto L = mara::get_cell_block(tree, index.prev_on(axis), mara::compose(nd::to_shared(), nd::select_final(guard_count, axis)));
        auto R = mara::get_cell_block(tree, index.next_on(axis), mara::compose(nd::to_shared(), nd::select_first(guard_count, axis)));
        return L | nd::concat(C).on_axis(axis) | nd::concat(R).on_axis(axis) | nd::to_shared();
    }, tree_launch);
};


template<typename TreeType>
static auto extend(TreeType my_tree, TreeType full_tree, std::size_t axis, std::size_t guard_count)
{
    return my_tree.indexes().map([=] (auto index)
    {
        auto block = my_tree.at(index);
        if (block.size() == 0)
            return block | nd::to_shared();

        auto C = full_tree.at(index);
        auto L = mara::get_cell_block(full_tree, index.prev_on(axis), mara::compose(nd::to_shared(), nd::select_final(guard_count, axis)));
        auto R = mara::get_cell_block(full_tree, index.next_on(axis), mara::compose(nd::to_shared(), nd::select_first(guard_count, axis)));
        return L | nd::concat(C).on_axis(axis) | nd::concat(R).on_axis(axis) | nd::to_shared();
    }, tree_launch);
};




//=============================================================================
template<typename PrimitiveArray>
static auto estimate_plm_difference(PrimitiveArray p0, std::size_t axis, double plm_theta)
{
    return p0
    | nd::zip_adjacent3_on_axis(axis)
    | nd::apply([plm_theta] (auto a, auto b, auto c) { return mara::plm_gradient(a, b, c, plm_theta); });
};




//=============================================================================
static auto cs2_at_position(binary::location_2d_t x, mara::two_body_state_t binary, const binary::solver_data_t& solver_data)
{
    auto M = solver_data.mach_number;

    if (solver_data.axisymmetric_cs2)
    {
        auto GM = 1.0;
        auto r2 = (x * x).sum().value;
        return GM / std::sqrt(r2) / M / M;
    }
    auto body1_pos = binary::location_2d_t{binary.body1.position_x, binary.body1.position_y};
    auto body2_pos = binary::location_2d_t{binary.body2.position_x, binary.body2.position_y};
    auto phi1 = grav_phi_field(solver_data, body1_pos, binary.body1.mass);
    auto phi2 = grav_phi_field(solver_data, body2_pos, binary.body2.mass);
    return -(phi1(x) + phi2(x)).value / M / M;
};

static auto nu_at_position(binary::location_2d_t x, double cs2, const binary::solver_data_t& solver_data)
{
    auto radius = [] (auto x) { return std::sqrt((x * x).sum().value); };
    auto profile = [radius, rc=solver_data.alpha_cutoff_radius] (auto x)
    {
        return rc > 0.0 ? 0.5 * (1.0 + std::tanh(3.0 * (radius(x) - rc))) : 1.0;
    };
    auto scale_height = [radius, M=solver_data.mach_number] (auto x)
    {
        return radius(x) / M;
    };
    if (solver_data.nu > 0.0)
    {
        return profile(x) * solver_data.nu;
    }
    return profile(x) * solver_data.alpha * std::sqrt(cs2) * scale_height(x);
}




//=============================================================================
static auto to_angmom_fluxes(std::size_t axis, mara::unit_length<double> domain_radius)
{
    return [axis, rd=domain_radius] (binary::location_2d_t x, mara::iso2d::flux_t f)
    {
        auto flux_sigma = mara::get<0>(f);
        auto flux_px    = mara::get<1>(f);
        auto flux_py    = mara::get<2>(f);
        auto flux_sr    = x[0] * flux_px + x[1] * flux_py;
        auto flux_lz    = x[0] * flux_py - x[1] * flux_px;

        if (axis == 0 && (x[0] == -rd || x[0] == rd)) flux_lz = 0.0;
        if (axis == 1 && (x[1] == -rd || x[1] == rd)) flux_lz = 0.0;

        return mara::make_arithmetic_tuple(flux_sigma, flux_sr, flux_lz);
    };
};




//=============================================================================
static mara::iso2d::flux_t viscous_flux(std::size_t axis,
    prim_pair_t g_long,
    prim_pair_t g_tran,
    double mu)
{
    auto [gl, gr] = g_long;
    auto [hl, hr] = g_tran;

    switch (axis)
    {
        case 0:
        {
            auto dx_ux = 0.5 * (gl.velocity_x() + gr.velocity_x());
            auto dx_uy = 0.5 * (gl.velocity_y() + gr.velocity_y());
            auto dy_ux = 0.5 * (hl.velocity_x() + hr.velocity_x());
            auto dy_uy = 0.5 * (hl.velocity_y() + hr.velocity_y());

            auto tauxx = mu * (dx_ux - dy_uy);
            auto tauxy = mu * (dx_uy + dy_ux);

            return mara::make_arithmetic_tuple(
                mara::make_dimensional<-1, 1, -1>(0.0),
                mara::make_dimensional< 0, 1,-2>(-tauxx),
                mara::make_dimensional< 0, 1,-2>(-tauxy));
        }
        case 1:
        {
            auto dx_ux = 0.5 * (hl.velocity_x() + hr.velocity_x());
            auto dx_uy = 0.5 * (hl.velocity_y() + hr.velocity_y());
            auto dy_ux = 0.5 * (gl.velocity_x() + gr.velocity_x());
            auto dy_uy = 0.5 * (gl.velocity_y() + gr.velocity_y());

            auto tauyx =  mu * (dx_uy + dy_ux);
            auto tauyy = -mu * (dx_ux - dy_uy);

            return mara::make_arithmetic_tuple(
                mara::make_dimensional<-1, 1, -1>(0.0),
                mara::make_dimensional< 0, 1,-2>(-tauyx),
                mara::make_dimensional< 0, 1,-2>(-tauyy));
        }
    }
    throw;
}




//=============================================================================
static auto intercell_flux_u(
    std::size_t axis,
    mara::two_body_state_t binary,
    binary::solver_data_t solver_data,
    mara::tree_index_t<2> tree_index)
{
    auto grid_spacing = 2.0 * solver_data.domain_radius.value / solver_data.block_size / (1 << tree_index.level);

    return [axis, binary, solver_data, grid_spacing] (binary::location_2d_t xf, prim_pair_t p0, prim_pair_t g_long, prim_pair_t g_tran)
    {
        auto [pl, pr] = p0;
        auto [gl, gr] = g_long;

        auto pl_hat = pl + gl * 0.5 * grid_spacing;
        auto pr_hat = pr - gr * 0.5 * grid_spacing;
        auto cs2 = cs2_at_position(xf, binary, solver_data);
        auto nu = nu_at_position(xf, cs2, solver_data);
        auto mu = 0.5 * nu * (pl_hat.sigma() + pr_hat.sigma());

        auto nhat = mara::unit_vector_t::on_axis(axis);
        auto fhat = mara::iso2d::riemann_hlle(pl_hat, pr_hat, cs2, cs2, nhat);
        auto fhat_visc = viscous_flux(axis, g_long, g_tran, mu);

        return fhat + fhat_visc;
    };
}

static auto intercell_flux_q(
    std::size_t axis,
    mara::two_body_state_t binary,
    binary::solver_data_t solver_data,
    mara::tree_index_t<2> tree_index)
{
    auto to_angmom = to_angmom_fluxes(axis, solver_data.domain_radius);
    auto grid_spacing = 2.0 * solver_data.domain_radius.value / solver_data.block_size / (1 << tree_index.level);

    return [axis, binary, solver_data, to_angmom, grid_spacing] (binary::location_2d_t xf, prim_pair_t p0, prim_pair_t g_long, prim_pair_t g_tran)
    {
        auto [pl, pr] = p0;
        auto [gl, gr] = g_long;

        auto pl_hat = pl + gl * 0.5 * grid_spacing;
        auto pr_hat = pr - gr * 0.5 * grid_spacing;
        auto cs2 = cs2_at_position(xf, binary, solver_data);
        auto nu = nu_at_position(xf, cs2, solver_data);
        auto mu = 0.5 * nu * (pl_hat.sigma() + pr_hat.sigma());

        auto nhat = mara::unit_vector_t::on_axis(axis);
        auto fhat = mara::iso2d::riemann_hlle(pl_hat, pr_hat, cs2, cs2, nhat);
        auto fhat_visc = viscous_flux(axis, g_long, g_tran, mu);

        return to_angmom(xf, fhat + fhat_visc);
    };
}




//=============================================================================
static auto force_to_source_terms_u(binary::location_2d_t x, force_per_area_t f)
{
    auto sigma_dot = mara::make_dimensional<-2, 1, -1>(0.0);
    return mara::make_arithmetic_tuple(sigma_dot, f[0], f[1]);
};

static auto force_to_source_terms_q(binary::location_2d_t x, force_per_area_t f)
{
    auto sigma_dot = mara::make_dimensional<-2, 1, -1>(0.0);
    auto sr_dot    = x[0] * f[0] + x[1] * f[1];
    auto lz_dot    = x[0] * f[1] - x[1] * f[0];
    return mara::make_arithmetic_tuple(sigma_dot, sr_dot, lz_dot);
};




//=============================================================================
static auto source_terms_u = [] (auto solver_data, auto solution, auto binary, auto p0, auto tree_index, auto dt)
{
    auto body1_pos = binary::location_2d_t{binary.body1.position_x, binary.body1.position_y};
    auto body2_pos = binary::location_2d_t{binary.body2.position_x, binary.body2.position_y};

    auto xc  = solver_data.cell_centers.at(tree_index);
    auto dA  = solver_data.cell_areas.at(tree_index);
    auto br  = solver_data.buffer_rate_field.at(tree_index);
    auto u0  = solution.conserved_u.at(tree_index);

    auto conserved_u_to_lz = [xc] (auto uc)
    {
        return nd::zip(uc, xc) | nd::apply([] (auto u, auto x)
        {
            return mara::iso2d::angular_momentum(u, x);
        });
    };

    auto sigma = u0 | component<0>();
    auto fg1 = (xc | nd::map(grav_vdot_field(solver_data, body1_pos, binary.body1.mass))) * sigma | nd::to_shared();
    auto fg2 = (xc | nd::map(grav_vdot_field(solver_data, body2_pos, binary.body2.mass))) * sigma | nd::to_shared();

    auto s_grav_1 = (nd::zip(xc, fg1) | nd::apply(force_to_source_terms_u)) * dt | nd::to_shared();
    auto s_grav_2 = (nd::zip(xc, fg2) | nd::apply(force_to_source_terms_u)) * dt | nd::to_shared();
    auto s_sink_1 = -u0 * (xc | nd::map(sink_rate_field(solver_data, body1_pos))) * dt | nd::to_shared();
    auto s_sink_2 = -u0 * (xc | nd::map(sink_rate_field(solver_data, body2_pos))) * dt | nd::to_shared();
    auto s_buffer = (solver_data.initial_conserved_u.at(tree_index) - u0) * br * dt | nd::to_shared();

    auto totals = binary::source_term_total_t();
    totals.mass_accreted_on[0]             = -(s_sink_1    | component<0>() | nd::multiply(dA) | nd::sum());
    totals.mass_accreted_on[1]             = -(s_sink_2    | component<0>() | nd::multiply(dA) | nd::sum());
    totals.angular_momentum_accreted_on[0] = -(s_sink_1    | conserved_u_to_lz | nd::multiply(dA) | nd::sum());
    totals.angular_momentum_accreted_on[1] = -(s_sink_2    | conserved_u_to_lz | nd::multiply(dA) | nd::sum());
    totals.integrated_torque_on[0]         = -(s_grav_1    | conserved_u_to_lz | nd::multiply(dA) | nd::sum());
    totals.integrated_torque_on[1]         = -(s_grav_2    | conserved_u_to_lz | nd::multiply(dA) | nd::sum());
    totals.angular_momentum_ejected        = -(s_buffer    | conserved_u_to_lz | nd::multiply(dA) | nd::sum());
    totals.mass_ejected                    = -(s_buffer    | component<0>() | nd::multiply(dA) | nd::sum());
    totals.integrated_force_x_on[0]        = -(fg1 * dt    | component<0>() | nd::multiply(dA) | nd::sum());
    totals.integrated_force_x_on[1]        = -(fg2 * dt    | component<0>() | nd::multiply(dA) | nd::sum());
    totals.integrated_force_y_on[0]        = -(fg1 * dt    | component<1>() | nd::multiply(dA) | nd::sum());
    totals.integrated_force_y_on[1]        = -(fg2 * dt    | component<1>() | nd::multiply(dA) | nd::sum());
    totals.momentum_x_accreted_on[0]       = -(s_sink_1    | component<1>() | nd::multiply(dA) | nd::sum());
    totals.momentum_x_accreted_on[1]       = -(s_sink_2    | component<1>() | nd::multiply(dA) | nd::sum());
    totals.momentum_y_accreted_on[0]       = -(s_sink_1    | component<2>() | nd::multiply(dA) | nd::sum());
    totals.momentum_y_accreted_on[1]       = -(s_sink_2    | component<2>() | nd::multiply(dA) | nd::sum());

    return std::make_pair((s_grav_1 + s_grav_2 + s_sink_1 + s_sink_2 + s_buffer) | nd::to_shared(), totals);
};




//=============================================================================
static auto source_terms_q = [] (auto solver_data, auto solution, auto binary, auto p0, auto tree_index, auto dt)
{
    auto body1_pos = binary::location_2d_t{binary.body1.position_x, binary.body1.position_y};
    auto body2_pos = binary::location_2d_t{binary.body2.position_x, binary.body2.position_y};

    auto sr2 = solver_data.gst_suppr_radius.template pow<2>();
    auto xc  = solver_data.cell_centers.at(tree_index);
    auto dA  = solver_data.cell_areas.at(tree_index);
    auto br  = solver_data.buffer_rate_field.at(tree_index);
    auto q0  = solution.conserved_q.at(tree_index);

    auto sigma = q0 | component<0>();
    auto fg1 = (xc | nd::map(grav_vdot_field(solver_data, body1_pos, binary.body1.mass))) * sigma | nd::to_shared();
    auto fg2 = (xc | nd::map(grav_vdot_field(solver_data, body2_pos, binary.body2.mass))) * sigma | nd::to_shared();

    auto s_grav_1 = (nd::zip(xc, fg1) | nd::apply(force_to_source_terms_q)) * dt | nd::to_shared();
    auto s_grav_2 = (nd::zip(xc, fg2) | nd::apply(force_to_source_terms_q)) * dt | nd::to_shared();
    auto s_sink_1 = -q0 * (xc | nd::map(sink_rate_field(solver_data, body1_pos))) * dt | nd::to_shared();
    auto s_sink_2 = -q0 * (xc | nd::map(sink_rate_field(solver_data, body2_pos))) * dt | nd::to_shared();
    auto s_buffer = (solver_data.initial_conserved_q.at(tree_index) - q0) * br * dt | nd::to_shared();
    auto s_geom = nd::zip(p0.at(tree_index), xc) | nd::apply([sr2, binary, solver_data, dt] (auto p, auto x)
    {
        auto ramp = 1.0 - std::exp(-(x * x).sum() / sr2);
        auto cs2 = cs2_at_position(x, binary, solver_data);
        return p.source_terms_conserved_angmom(cs2) * ramp * dt;
    }) | nd::to_shared();

    auto dps1 = nd::zip(s_sink_1, xc) | nd::apply(mara::iso2d::to_conserved_per_area) | nd::map(mara::iso2d::momentum_vector) | nd::to_shared();
    auto dps2 = nd::zip(s_sink_2, xc) | nd::apply(mara::iso2d::to_conserved_per_area) | nd::map(mara::iso2d::momentum_vector) | nd::to_shared();

    auto totals = binary::source_term_total_t();
    totals.mass_accreted_on[0]             = -(s_sink_1    | component<0>() | nd::multiply(dA) | nd::sum());
    totals.mass_accreted_on[1]             = -(s_sink_2    | component<0>() | nd::multiply(dA) | nd::sum());
    totals.angular_momentum_accreted_on[0] = -(s_sink_1    | component<2>() | nd::multiply(dA) | nd::sum());
    totals.angular_momentum_accreted_on[1] = -(s_sink_2    | component<2>() | nd::multiply(dA) | nd::sum());
    totals.integrated_torque_on[0]         = -(s_grav_1    | component<2>() | nd::multiply(dA) | nd::sum());
    totals.integrated_torque_on[1]         = -(s_grav_2    | component<2>() | nd::multiply(dA) | nd::sum());
    totals.mass_ejected                    = -(s_buffer    | component<0>() | nd::multiply(dA) | nd::sum());
    totals.angular_momentum_ejected        = -(s_buffer    | component<2>() | nd::multiply(dA) | nd::sum());
    totals.integrated_force_x_on[0]        = -(fg1 * dt    | component<0>() | nd::multiply(dA) | nd::sum());
    totals.integrated_force_x_on[1]        = -(fg2 * dt    | component<0>() | nd::multiply(dA) | nd::sum());
    totals.integrated_force_y_on[0]        = -(fg1 * dt    | component<1>() | nd::multiply(dA) | nd::sum());
    totals.integrated_force_y_on[1]        = -(fg2 * dt    | component<1>() | nd::multiply(dA) | nd::sum());
    totals.momentum_x_accreted_on[0]       = -(dps1        | component<0>() | nd::multiply(dA) | nd::sum());
    totals.momentum_x_accreted_on[1]       = -(dps2        | component<0>() | nd::multiply(dA) | nd::sum());
    totals.momentum_y_accreted_on[0]       = -(dps1        | component<1>() | nd::multiply(dA) | nd::sum());
    totals.momentum_y_accreted_on[1]       = -(dps2        | component<1>() | nd::multiply(dA) | nd::sum());

    return std::make_pair((s_grav_1 + s_grav_2 + s_sink_1 + s_sink_2 + s_buffer + s_geom) | nd::to_shared(), totals);
};




//=============================================================================
static auto block_fluxes_u = [] (
    auto solver_data,
    auto solution,
    auto binary,
    auto p0,
    auto p0_ex,
    auto p0_ey,
    auto gx_ex,
    auto gx_ey,
    auto gy_ex,
    auto gy_ey,
    auto dt)
{
    return [=] (auto tree_index)
    {
        if (solution.conserved_u.at(tree_index).size() == 0)
        {
            auto flux = nd::shared_array<mara::iso2d::flux_t, 2>() * mara::make_length(1.0) | nd::to_shared();
            return std::make_tuple(flux, flux);
        }

        auto u0 = solution.conserved_u.at(tree_index);
        auto xv = solver_data.vertices.at(tree_index);
        auto xc = solver_data.cell_centers.at(tree_index);
        auto dA = solver_data.cell_areas.at(tree_index);
        auto dx = xv | component<0>() | nd::difference_on_axis(0);
        auto dy = xv | component<1>() | nd::difference_on_axis(1);
        auto xf = xv | nd::midpoint_on_axis(1);
        auto yf = xv | nd::midpoint_on_axis(0);

        auto fhat_x = nd::zip(
            xf,
            p0_ex.at(tree_index) | nd::zip_adjacent2_on_axis(0),
            gx_ex.at(tree_index) | nd::zip_adjacent2_on_axis(0),
            gy_ex.at(tree_index) | nd::zip_adjacent2_on_axis(0))
        | nd::apply(intercell_flux_u(0, binary, solver_data, tree_index))
        | nd::multiply(dy)
        | nd::to_shared();

        auto fhat_y = nd::zip(
            yf,
            p0_ey.at(tree_index) | nd::zip_adjacent2_on_axis(1),
            gy_ey.at(tree_index) | nd::zip_adjacent2_on_axis(1),
            gx_ey.at(tree_index) | nd::zip_adjacent2_on_axis(1))
        | nd::apply(intercell_flux_u(1, binary, solver_data, tree_index))
        | nd::multiply(dx)
        | nd::to_shared();

        return std::make_tuple(fhat_x, fhat_y);
    };
};

static auto block_fluxes_q = [] (
    auto solver_data,
    auto solution,
    auto binary,
    auto p0,
    auto p0_ex,
    auto p0_ey,
    auto gx_ex,
    auto gx_ey,
    auto gy_ex,
    auto gy_ey,
    auto dt)
{
    return [=] (auto tree_index)
    {
        if (solution.conserved_q.at(tree_index).size() == 0)
        {
            auto flux = nd::shared_array<mara::iso2d::flux_angmom_t, 2>() * mara::make_length(1.0) | nd::to_shared();
            return std::make_tuple(flux, flux);
        }

        auto q0 = solution.conserved_q.at(tree_index);
        auto xv = solver_data.vertices.at(tree_index);
        auto xc = solver_data.cell_centers.at(tree_index);
        auto dA = solver_data.cell_areas.at(tree_index);
        auto dx = xv | component<0>() | nd::difference_on_axis(0);
        auto dy = xv | component<1>() | nd::difference_on_axis(1);
        auto xf = xv | nd::midpoint_on_axis(1);
        auto yf = xv | nd::midpoint_on_axis(0);

        auto fhat_x = nd::zip(
            xf,
            p0_ex.at(tree_index) | nd::zip_adjacent2_on_axis(0),
            gx_ex.at(tree_index) | nd::zip_adjacent2_on_axis(0),
            gy_ex.at(tree_index) | nd::zip_adjacent2_on_axis(0))
        | nd::apply(intercell_flux_q(0, binary, solver_data, tree_index))
        | nd::multiply(dy)
        | nd::to_shared();

        auto fhat_y = nd::zip(
            yf,
            p0_ey.at(tree_index) | nd::zip_adjacent2_on_axis(1),
            gy_ey.at(tree_index) | nd::zip_adjacent2_on_axis(1),
            gx_ey.at(tree_index) | nd::zip_adjacent2_on_axis(1))
        | nd::apply(intercell_flux_q(1, binary, solver_data, tree_index))
        | nd::multiply(dx)
        | nd::to_shared();

        return std::make_tuple(fhat_x, fhat_y);
    };
};




//=============================================================================
static auto block_update_u = [] (
    auto solver_data,
    auto solution,
    auto binary,
    auto p0,
    auto fhat_x,
    auto fhat_y,
    auto dt)
{
    return [=] (auto tree_index)
    {
        auto dA = solver_data.cell_areas.at(tree_index);
        auto u0 = solution.conserved_u.at(tree_index);

        if (u0.size() == 0)
        {
            return std::make_pair(u0.shared(), binary::source_term_total_t());
        }
        auto lx = fhat_x.at(tree_index) | nd::difference_on_axis(0);
        auto ly = fhat_y.at(tree_index) | nd::difference_on_axis(1);
        auto s  = source_terms_u(solver_data, solution, binary, p0, tree_index, dt);
        return std::make_pair((u0 - (lx + ly) * dt / dA + s.first) | nd::to_shared(), s.second);
    };
};

static auto block_update_q = [] (
    auto solver_data,
    auto solution,
    auto binary,
    auto p0,
    auto fhat_x,
    auto fhat_y,
    auto dt)
{
    return [=] (auto tree_index)
    {
        auto dA = solver_data.cell_areas.at(tree_index);
        auto q0 = solution.conserved_q.at(tree_index);

        if (q0.size() == 0)
        {
            return std::make_pair(q0.shared(), binary::source_term_total_t());
        }
        auto lx = fhat_x.at(tree_index) | nd::difference_on_axis(0);
        auto ly = fhat_y.at(tree_index) | nd::difference_on_axis(1);
        auto s  = source_terms_q(solver_data, solution, binary, p0, tree_index, dt);
        return std::make_pair((q0 - (lx + ly) * dt / dA + s.first) | nd::to_shared(), s.second);
    };
};




//=============================================================================
static auto correct_fluxes_xl = [] (auto fhat_tree, mara::tree_index_t<2> index)
{
    return [fhat_tree, index] (auto fhat)
    {
        if (fhat_tree.contains(index.prev_on(0)))
            return fhat;

        if (fhat_tree.contains(index.prev_on(0).parent_index()))
            return fhat;

        auto nodel = fhat_tree.node_at(index.prev_on(0));
        auto fluxl = nodel.at({1, {1, 0}})
        | nd::concat(nodel.at({1, {1, 1}})).on_axis(1)
        | mara::restrict_extrinsic(1)
        | nd::take_final_on_axis(0);

        auto fluxr = fhat | nd::drop_first_on_axis(0);
        return fluxl | nd::concat(fluxr).on_axis(0) | nd::to_shared();
    };
};

static auto correct_fluxes_xr = [] (auto fhat_tree, mara::tree_index_t<2> index)
{
    return [fhat_tree, index] (auto fhat)
    {
        if (fhat_tree.contains(index.next_on(0)))
            return fhat;

        if (fhat_tree.contains(index.next_on(0).parent_index()))
            return fhat;

        auto noder = fhat_tree.node_at(index.next_on(0));
        auto fluxr = noder.at({1, {0, 0}})
        | nd::concat(noder.at({1, {0, 1}})).on_axis(1)
        | mara::restrict_extrinsic(1)
        | nd::take_first_on_axis(0);

        auto fluxl = fhat | nd::drop_final_on_axis(0);
        return fluxl | nd::concat(fluxr).on_axis(0) | nd::to_shared();
    };
};

static auto correct_fluxes_yl = [] (auto fhat_tree, mara::tree_index_t<2> index)
{
    return [fhat_tree, index] (auto fhat)
    {
        if (fhat_tree.contains(index.prev_on(1)))
            return fhat;

        if (fhat_tree.contains(index.prev_on(1).parent_index()))
            return fhat;

        auto nodel = fhat_tree.node_at(index.prev_on(1));
        auto fluxl = nodel.at({1, {0, 1}})
        | nd::concat(nodel.at({1, {1, 1}})).on_axis(0)
        | mara::restrict_extrinsic(0)
        | nd::take_final_on_axis(1);

        auto fluxr = fhat | nd::drop_first_on_axis(1);
        return fluxl | nd::concat(fluxr).on_axis(1) | nd::to_shared();
    };
};

static auto correct_fluxes_yr = [] (auto fhat_tree, mara::tree_index_t<2> index)
{
    return [fhat_tree, index] (auto fhat)
    {
        if (fhat_tree.contains(index.next_on(1)))
            return fhat;

        if (fhat_tree.contains(index.next_on(1).parent_index()))
            return fhat;

        auto noder = fhat_tree.node_at(index.next_on(1));
        auto fluxr = noder.at({1, {0, 0}})
        | nd::concat(noder.at({1, {1, 0}})).on_axis(0)
        | mara::restrict_extrinsic(0)
        | nd::take_first_on_axis(1);

        auto fluxl = fhat | nd::drop_final_on_axis(1);
        return fluxl | nd::concat(fluxr).on_axis(1) | nd::to_shared();
    };
};


//=============================================================================
auto correct_fluxes_x = [] (auto fx_full)
{
    return [fx_full] (auto index_block_pair)
    {
        auto [index, flux_block] = index_block_pair;

        if (flux_block.size() == 0)
            return flux_block;

        return flux_block
        | correct_fluxes_xl(fx_full, index)
        | correct_fluxes_xr(fx_full, index);
    };
};

auto correct_fluxes_y = [] (auto fy_full)
{
    return [fy_full] (auto index_block_pair)
    {
        auto [index, flux_block] = index_block_pair;

        if (flux_block.size() == 0)
            return flux_block;

        return flux_block
        | correct_fluxes_yl(fy_full, index)
        | correct_fluxes_yr(fy_full, index);
    };
};




//=============================================================================
auto validate_q = [] (auto solution, auto solver_data)
{
    bool any_failures = false;

    solution.conserved_q.indexes().sink([&any_failures, solution, solver_data] (auto index)
    {
        auto XQ = nd::zip(solver_data.cell_centers.at(index), solution.conserved_q.at(index));

        for (auto xq : XQ)
        {
            const auto& x = std::get<0>(xq);
            const auto& q = std::get<1>(xq);

            if (mara::get<0>(q) < 0.0)
            {                
                std::printf("negative density %3.2e (at position [%+3.2lf %+3.2lf])\n", mara::get<0>(q).value, x[0].value, x[1].value);
                any_failures = true;
            }
        }
    });

    if (any_failures)
    {
        // std::printf("\t rank (%d) throwing error!\n", mpi::comm_world().rank());
        // std::printf("\t throwing error!\n");
        auto error_string = "negative density in updated state " + std::to_string(mpi::comm_world().rank());
        throw std::runtime_error(error_string);
    }
    return solution;
};

auto validate_u = [] (auto solution, auto solver_data)
{
    bool any_failures = false;

    solution.conserved_u.indexes().sink([&any_failures, solution, solver_data] (auto index)
    {
        auto XQ = nd::zip(solver_data.cell_centers.at(index), solution.conserved_u.at(index));

        for (auto xq : XQ)
        {
            const auto& x = std::get<0>(xq);
            const auto& q = std::get<1>(xq);

            if (mara::get<0>(q) < 0.0)
            {                
                std::printf("negative density %3.2e (at position [%+3.2lf %+3.2lf])\n", mara::get<0>(q).value, x[0].value, x[1].value);
                any_failures = true;
            }
        }
    });

    if (any_failures)
    {
        // std::printf("\t rank (%d) throwing error!\n", mpi::comm_world().rank());
        // std::printf("\t throwing error!\n");
        auto error_string = "negative density in updated state " + std::to_string(mpi::comm_world().rank());
        throw std::runtime_error(error_string);
    }
    return solution;
};






//=============================================================================
//     Put all of this in `subprog_binary_parallel.cpp` when know how...
//=============================================================================

binary::rank_tree_t binary::create_rank_tree(const mara::config_t& run_config)
{
    return  mara::build_rank_tree<2>(create_vertices(run_config).indexes(), mpi::comm_world().size());
}

//=============================================================================
template<typename Iterable, typename Predicate>
auto filter(const Iterable& container, Predicate predicate)
{
    using value_type = std::decay_t<decltype(*container.begin())>;
    
    auto result = std::vector<value_type>();

    for (auto i : container)
        if (predicate(i))
            result.push_back(i);

    return result;
}


template<typename Iterable, typename Predicate>
auto remove_if(const Iterable& container, Predicate predicate)
{
    return filter(container, [predicate] (auto v) { return ! predicate(v); });
}


template<typename Iterable>
auto linked_list_from(const Iterable& container)
{
    using value_type = std::decay_t<decltype(*container.begin())>;
    return mara::linked_list_t<value_type>(container.begin(), container.end());
}



//=============================================================================
/**
 * @return   Returns a boolean function (index) -> bool that gives
 *           true if my process owns the index
 */
auto block_is_owned_by(std::size_t rank, const binary::rank_tree_t& rank_tree)
{
    return [rank, rank_tree] (auto index)
    {
        return rank_tree.at(index) == rank;
    };
};




/**
 * @brief   Return a unique vector of all the indexes my process needs in order
 *          to update all of the blocks that I own
 */
std::vector<mara::tree_index_t<2>> binary::indexes_of_nonlocal_blocks(const binary::rank_tree_t& rank_tree)
{
    /**
     * @brief   Gives the index at target, it's parent, or all 4 of its children
     */
    auto indexes_at = [] (auto tree, auto target)
    {
        if (tree.contains(target))
            return mara::linked_list_t<mara::tree_index_t<2>>().prepend(target);

        if (tree.contains(target.parent_index()))
            return mara::linked_list_t<mara::tree_index_t<2>>().prepend(target.parent_index());

        return linked_list_from(tree.indexes().node_at(target));
    };


    /**
     * @brief   Get all neighbor indexes to a given index
    */ 
    auto get_neighbors_at = [indexes_at] (auto tree, auto idx)
    {
        return  indexes_at(tree, idx.next_on(0))
        .concat(indexes_at(tree, idx.prev_on(0)))
        .concat(indexes_at(tree, idx.next_on(1)))
        .concat(indexes_at(tree, idx.prev_on(1)));
    };


    // can fix this to make it more logical/efficient but I think it works for now
    //=========================================================================
    auto my_rank   = mpi::comm_world().rank();
    auto indexes   = mara::linked_list_t<mara::tree_index_t<2>>();

    for (auto ir : rank_tree.pair_indexes())
    {
        auto idx  = ir.first;
        auto rank = ir.second;

        if (rank != my_rank)
            continue;

        indexes = indexes.concat(get_neighbors_at(rank_tree, idx));
    }
    return remove_if(indexes.unique(), block_is_owned_by(my_rank, rank_tree));
}


template<typename ValueType>
binary::quad_tree_t<ValueType> binary::mpi_fill_tree(quad_tree_t<ValueType> block_tree, const solver_data_t& solver_data)
{
    using message_type_t = std::pair<mara::tree_index_t<2>, nd::shared_array<ValueType, 2>>;

    auto comm             = mpi::comm_world();
    auto rank_tree        = solver_data.domain_decomposition;

    // auto indexes_to_recv  = indexes_of_nonlocal_blocks(rank_tree);       
    auto indexes_to_recv  = solver_data.indexes_to_recv;
    // auto indexes_to_send  = solver_data.indexes_to_send;
    auto indexes_to_send  = comm.all_gather(indexes_to_recv);
    auto requests         = std::vector<mpi::Request>();

    for (auto rank : filter(nd::arange(comm.size()), [rank = comm.rank()] (auto r) { return r != rank; }))
        for (auto i : filter(indexes_to_send[rank], block_is_owned_by(comm.rank(), rank_tree)))
            requests.push_back(comm.isend(mara::dumps(std::pair(i, block_tree.at(i))), rank, 0));

    for (auto index : indexes_to_recv)
    {
        auto [idx, block] = mara::loads<message_type_t>(comm.recv(rank_tree.at(index), 0));
        block_tree = block_tree.insert(idx, block);
    }

    comm.barrier(); // This barrier ensures all processes have completed their
                    // non-blocking sends before the requests go out of scope.

    for (const auto& request : requests)
        if (! request.is_ready())
            throw std::logic_error("A send request was not completed");

    return block_tree;
}


binary::solution_t binary::mpi_reduce_sources(const solution_t& solution)
{
    auto comm = mpi::comm_world();

    auto global_mass_accreted_on = mara::arithmetic_sequence_t<mara::unit_mass<double>, 2>{
        comm.all_reduce(solution.mass_accreted_on[0], mpi::operation::sum),
        comm.all_reduce(solution.mass_accreted_on[1], mpi::operation::sum)};   

    auto global_angular_momentum_accreted_on = mara::arithmetic_sequence_t<mara::unit_angmom<double>, 2>{
        comm.all_reduce(solution.angular_momentum_accreted_on[0], mpi::operation::sum),
        comm.all_reduce(solution.angular_momentum_accreted_on[1], mpi::operation::sum)}; 

    auto global_integrated_torque_on = mara::arithmetic_sequence_t<mara::unit_angmom<double>, 2>{
        comm.all_reduce(solution.integrated_torque_on[0], mpi::operation::sum),
        comm.all_reduce(solution.integrated_torque_on[1], mpi::operation::sum)}; 

    auto global_work_done_on = mara::arithmetic_sequence_t<mara::unit_energy<double>, 2>{
        comm.all_reduce(solution.work_done_on[0], mpi::operation::sum),
        comm.all_reduce(solution.work_done_on[1], mpi::operation::sum)}; 

    auto global_mass_ejected             = comm.all_reduce(solution.mass_ejected, mpi::operation::sum);
    auto global_angular_momentum_ejected = comm.all_reduce(solution.angular_momentum_ejected, mpi::operation::sum);
    auto global_orbital_elements_acc     = comm.all_reduce(solution.orbital_elements_acc,  mpi::operation::sum);
    auto global_orbital_elements_grav    = comm.all_reduce(solution.orbital_elements_grav, mpi::operation::sum);

    return solution_t{
        solution.time,
        solution.iteration,
        solution.conserved_u,
        solution.conserved_q,
        global_mass_accreted_on,
        global_angular_momentum_accreted_on,
        global_integrated_torque_on,
        global_work_done_on,
        global_mass_ejected,
        global_angular_momentum_ejected,
        global_orbital_elements_acc,
        global_orbital_elements_grav,
    };
}

//=============================================================================
//=============================================================================




//=============================================================================
binary::solution_t binary::advance_u(const solution_t& solution, const solver_data_t& solver_data, mara::unit_time<double> dt, bool safe_mode)
{
    auto th = safe_mode ? 0.0 : solver_data.plm_theta;
    auto spacing_at_root = 2.0 * solver_data.domain_radius.value / solver_data.block_size;
    auto estimate_gradient = [th, spacing_at_root] (std::size_t axis)
    {
        return [th, spacing_at_root, axis] (mara::tree_index_t<2> tree_index, auto p)
        {
            auto spacing = spacing_at_root / (1 << tree_index.level);
            return (estimate_plm_difference(p, axis, th) / spacing) | nd::to_shared();
        };
    };

    // Compute pre-requisite data
    //=========================================================================
    auto p0      = recover_primitive(solution, solver_data);
    auto p0_full = binary::mpi_fill_tree(p0, solver_data);
    auto p0_ex   = extend(p0, p0_full, 0, 1);
    auto p0_ey   = extend(p0, p0_full, 1, 1);

    auto gx      = p0_ex.pair_indexes().apply(estimate_gradient(0), tree_launch);
    auto gy      = p0_ey.pair_indexes().apply(estimate_gradient(1), tree_launch);
    auto gx_full = binary::mpi_fill_tree(gx, solver_data);
    auto gy_full = binary::mpi_fill_tree(gy, solver_data);
    auto gx_ex   = extend(gx, gx_full, 0, 1);
    auto gx_ey   = extend(gx, gx_full, 1, 1);
    auto gy_ex   = extend(gy, gy_full, 0, 1);
    auto gy_ey   = extend(gy, gy_full, 1, 1);
    
    auto binary = mara::compute_two_body_state(solver_data.binary_params, solution.time.value);

    auto fhat = p0
    .indexes()
    .map(block_fluxes_u(solver_data, solution, binary, p0, p0_ex, p0_ey, gx_ex, gx_ey, gy_ex, gy_ey, dt), tree_launch);

    auto fhat_x  = fhat.map([] (auto t) { return std::get<0>(t); });
    auto fhat_y  = fhat.map([] (auto t) { return std::get<1>(t); });
    auto fx_full = binary::mpi_fill_tree(fhat_x, solver_data);
    auto fy_full = binary::mpi_fill_tree(fhat_y, solver_data);
    auto fhat_xc = fhat_x.pair_indexes().map(correct_fluxes_x(fx_full));
    auto fhat_yc = fhat_y.pair_indexes().map(correct_fluxes_y(fy_full));

    auto block_results = p0
    .indexes()
    .map(block_update_u(solver_data, solution, binary, p0, fhat_xc, fhat_yc, dt), tree_launch);

    auto u1     = block_results.map([] (const auto& t) { return t.first; });
    auto totals = block_results.map([] (const auto& t) { return t.second; }).sum();

    auto body1_acc = mara::point_mass_t{
        binary.body1.mass       + totals.mass_accreted_on[0].value,
        binary.body1.position_x,
        binary.body1.position_y,
        binary.body1.velocity_x + totals.momentum_x_accreted_on[0].value / binary.body1.mass,
        binary.body1.velocity_y + totals.momentum_y_accreted_on[0].value / binary.body1.mass,
    };

    auto body2_acc = mara::point_mass_t{
        binary.body2.mass       + totals.mass_accreted_on[1].value,
        binary.body2.position_x,
        binary.body2.position_y,
        binary.body2.velocity_x + totals.momentum_x_accreted_on[1].value / binary.body2.mass,
        binary.body2.velocity_y + totals.momentum_y_accreted_on[1].value / binary.body2.mass,
    };

    auto body1_grav = mara::point_mass_t{
        binary.body1.mass,
        binary.body1.position_x,
        binary.body1.position_y,
        binary.body1.velocity_x + totals.integrated_force_x_on[0].value / binary.body1.mass,
        binary.body1.velocity_y + totals.integrated_force_y_on[0].value / binary.body1.mass,
    };

    auto body2_grav = mara::point_mass_t{
        binary.body2.mass,
        binary.body2.position_x,
        binary.body2.position_y,
        binary.body2.velocity_x + totals.integrated_force_x_on[1].value / binary.body2.mass,
        binary.body2.velocity_y + totals.integrated_force_y_on[1].value / binary.body2.mass,
    };

    auto E0                  = mara::compute_orbital_elements(binary);
    auto delta_E_prime_acc   = mara::compute_orbital_elements({body1_acc,  body2_acc})  - E0;
    auto delta_E_prime_grav  = mara::compute_orbital_elements({body1_grav, body2_grav}) - E0;

    // The full updated solution state
    //=========================================================================
    auto full_solution = binary::mpi_reduce_sources(solution_t{
        solution.time + dt,
        solution.iteration + 1,
        u1,
        {},
        solution.mass_accreted_on             + totals.mass_accreted_on,
        solution.angular_momentum_accreted_on + totals.angular_momentum_accreted_on,
        solution.integrated_torque_on         + totals.integrated_torque_on,
        solution.work_done_on,
        solution.mass_ejected                 + totals.mass_ejected,
        solution.angular_momentum_ejected     + totals.angular_momentum_ejected,
        solution.orbital_elements_acc         + delta_E_prime_acc,
        solution.orbital_elements_grav        + delta_E_prime_grav,
    });
    mpi::comm_world().barrier();

    return validate_u(full_solution, solver_data);
}

binary::solution_t binary::advance_q(const solution_t& solution, const solver_data_t& solver_data, mara::unit_time<double> dt, bool safe_mode)
{

    auto th = safe_mode ? 0.0 : solver_data.plm_theta;
    auto spacing_at_root = 2.0 * solver_data.domain_radius.value / solver_data.block_size;
    auto estimate_gradient = [th, spacing_at_root] (std::size_t axis)
    {
        return [th, spacing_at_root, axis] (mara::tree_index_t<2> tree_index, auto p)
        {
            auto spacing = spacing_at_root / (1 << tree_index.level);
            return (estimate_plm_difference(p, axis, th) / spacing) | nd::to_shared();
        };
    };


    // Compute pre-requisite data
    //=========================================================================
    auto p0      = recover_primitive(solution, solver_data);
    auto p0_full = binary::mpi_fill_tree(p0, solver_data);
    auto p0_ex   = extend(p0, p0_full, 0, 1);
    auto p0_ey   = extend(p0, p0_full, 1, 1);

    auto gx      = p0_ex.pair_indexes().apply(estimate_gradient(0), tree_launch);
    auto gy      = p0_ey.pair_indexes().apply(estimate_gradient(1), tree_launch);
    auto gx_full = binary::mpi_fill_tree(gx, solver_data);
    auto gy_full = binary::mpi_fill_tree(gy, solver_data);
    auto gx_ex   = extend(gx, gx_full, 0, 1);
    auto gx_ey   = extend(gx, gx_full, 1, 1);
    auto gy_ex   = extend(gy, gy_full, 0, 1);
    auto gy_ey   = extend(gy, gy_full, 1, 1);
    auto binary = mara::compute_two_body_state(solver_data.binary_params, solution.time.value);

    auto fhat = p0
    .indexes()
    .map(block_fluxes_q(solver_data, solution, binary, p0, p0_ex, p0_ey, gx_ex, gx_ey, gy_ex, gy_ey, dt), tree_launch);

    auto fhat_x  = fhat.map([] (auto t) { return std::get<0>(t); });
    auto fhat_y  = fhat.map([] (auto t) { return std::get<1>(t); });
    auto fx_full = binary::mpi_fill_tree(fhat_x, solver_data);
    auto fy_full = binary::mpi_fill_tree(fhat_y, solver_data);
    auto fhat_xc = fhat_x.pair_indexes().map(correct_fluxes_x(fx_full));
    auto fhat_yc = fhat_y.pair_indexes().map(correct_fluxes_y(fy_full));

    auto block_results = p0
    .indexes()
    .map(block_update_q(solver_data, solution, binary, p0, fhat_xc, fhat_yc, dt), tree_launch);

    auto q1     = block_results.map([] (const auto& t) { return t.first; });
    auto totals = block_results.map([] (const auto& t) { return t.second; }).sum();

    auto body1_acc = mara::point_mass_t{
        binary.body1.mass       + totals.mass_accreted_on[0].value,
        binary.body1.position_x,
        binary.body1.position_y,
        binary.body1.velocity_x + totals.momentum_x_accreted_on[0].value / binary.body1.mass,
        binary.body1.velocity_y + totals.momentum_y_accreted_on[0].value / binary.body1.mass,
    };

    auto body2_acc = mara::point_mass_t{
        binary.body2.mass       + totals.mass_accreted_on[1].value,
        binary.body2.position_x,
        binary.body2.position_y,
        binary.body2.velocity_x + totals.momentum_x_accreted_on[1].value / binary.body2.mass,
        binary.body2.velocity_y + totals.momentum_y_accreted_on[1].value / binary.body2.mass,
    };

    auto body1_grav = mara::point_mass_t{
        binary.body1.mass,
        binary.body1.position_x,
        binary.body1.position_y,
        binary.body1.velocity_x + totals.integrated_force_x_on[0].value / binary.body1.mass,
        binary.body1.velocity_y + totals.integrated_force_y_on[0].value / binary.body1.mass,
    };

    auto body2_grav = mara::point_mass_t{
        binary.body2.mass,
        binary.body2.position_x,
        binary.body2.position_y,
        binary.body2.velocity_x + totals.integrated_force_x_on[1].value / binary.body2.mass,
        binary.body2.velocity_y + totals.integrated_force_y_on[1].value / binary.body2.mass,
    };

    auto E0                  = mara::compute_orbital_elements(binary);
    auto delta_E_prime_acc   = mara::compute_orbital_elements({body1_acc,  body2_acc})  - E0;
    auto delta_E_prime_grav  = mara::compute_orbital_elements({body1_grav, body2_grav}) - E0;


    // The full updated solution state
    //=========================================================================
    auto full_solution = mpi_reduce_sources(solution_t{
            solution.time + dt,
            solution.iteration + 1,
            {},
            q1,
            solution.mass_accreted_on             + totals.mass_accreted_on,
            solution.angular_momentum_accreted_on + totals.angular_momentum_accreted_on,
            solution.integrated_torque_on         + totals.integrated_torque_on,
            solution.work_done_on,
            solution.mass_ejected                 + totals.mass_ejected,
            solution.angular_momentum_ejected     + totals.angular_momentum_ejected,
            solution.orbital_elements_acc         + delta_E_prime_acc,
            solution.orbital_elements_grav        + delta_E_prime_grav
    });
    mpi::comm_world().barrier();

    return validate_q(full_solution, solver_data);
}

binary::solution_t binary::advance(const solution_t& solution, const solver_data_t& solver_data, mara::unit_time<double> dt, bool safe_mode)
{
    return solver_data.conserve_linear_p
    ? advance_u(solution, solver_data, dt, safe_mode)
    : advance_q(solution, solver_data, dt, safe_mode);
}




//=============================================================================
binary::solution_t binary::solution_t::operator+(const solution_t& other) const
{
    return {
        time         + other.time,
        iteration    + other.iteration,
        (conserved_u + other.conserved_u).map(nd::to_shared(), tree_launch),
        (conserved_q + other.conserved_q).map(nd::to_shared(), tree_launch),
        mass_accreted_on               + other.mass_accreted_on,
        angular_momentum_accreted_on   + other.angular_momentum_accreted_on,
        integrated_torque_on           + other.integrated_torque_on,
        work_done_on                   + other.work_done_on,
        mass_ejected                   + other.mass_ejected,
        angular_momentum_ejected       + other.angular_momentum_ejected,
        orbital_elements_acc           + other.orbital_elements_acc,
        orbital_elements_grav          + other.orbital_elements_grav,
    };
}

binary::solution_t binary::solution_t::operator*(mara::rational_number_t scale) const
{
    return {
        time         * scale.as_double(),
        iteration    * scale,
        (conserved_u * scale.as_double()).map(nd::to_shared(), tree_launch),
        (conserved_q * scale.as_double()).map(nd::to_shared(), tree_launch),
        mass_accreted_on               * scale.as_double(),
        angular_momentum_accreted_on   * scale.as_double(),
        integrated_torque_on           * scale.as_double(),
        work_done_on                   * scale.as_double(),
        mass_ejected                   * scale.as_double(),
        angular_momentum_ejected       * scale.as_double(),
        orbital_elements_acc           * scale.as_double(),
        orbital_elements_grav          * scale.as_double(),
    };
}




//=============================================================================
binary::quad_tree_t<mara::iso2d::primitive_t> binary::recover_primitive(
    const solution_t& solution,
    const solver_data_t& solver_data)
{
    auto recover_primitive_u = [] (auto U)
    {
        return U
        | nd::map([] (auto u) { return mara::iso2d::recover_primitive(u); })
        | nd::to_shared();
    };

    auto recover_primitive_q = [] (auto Q, auto X)
    {
        return nd::zip(Q, X)
        | nd::apply([] (auto q, auto x) { return mara::iso2d::recover_primitive(q, x); })
        | nd::to_shared();
    };

    return solver_data.conserve_linear_p

    ? solution.conserved_u
    .map(recover_primitive_u, tree_launch)

    : solution.conserved_q
    .pair(solver_data.cell_centers)
    .apply(recover_primitive_q, tree_launch);
}

#endif // MARA_COMPILE_SUBPROGRAM_BINARY
