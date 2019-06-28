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
#include <cmath>
#include <iostream>
#include "core_hdf5.hpp"
#include "core_ndarray.hpp"
#include "core_tree.hpp"
#include "core_rational.hpp"
#include "app_compile_opts.hpp"
#include "app_config.hpp"
#include "app_performance.hpp"
#include "app_schedule.hpp"
#include "app_serialize.hpp"
#include "physics_iso2d.hpp"
#include "model_two_body.hpp"




//=============================================================================
namespace binary
{


    //=========================================================================
    using location_2d_t = mara::arithmetic_sequence_t<mara::dimensional_value_t<1, 0,  0, double>, 2>;
    using velocity_2d_t = mara::arithmetic_sequence_t<mara::dimensional_value_t<1, 0, -1, double>, 2>;
    using accel_2d_t    = mara::arithmetic_sequence_t<mara::dimensional_value_t<1, 0, -2, double>, 2>;
    using force_2d_t    = mara::arithmetic_sequence_t<mara::dimensional_value_t<1, 1, -2, double>, 2>;
    using primitive_field_t = std::function<mara::iso2d::primitive_t(location_2d_t)>;

    template<typename ArrayValueType>
    using quad_tree_t = mara::arithmetic_binary_tree_t<nd::shared_array<ArrayValueType, 2>, 2>;


    //=========================================================================
    enum class reconstruct_method_t
    {
        plm,
        pcm,
    };


    //=========================================================================
    enum class riemann_solver_t
    {
        hlle,
        hllc,
    };


    //=========================================================================
    struct solver_data_t
    {
        mara::unit_rate  <double>                      sink_rate;
        mara::unit_time  <double>                      recommended_time_step;
        mara::unit_length<double>                      softening_radius;
        mara::unit_length<double>                      sink_radius;

        double                                         mach_number;
        double                                         plm_theta;
        int                                            rk_order;
        reconstruct_method_t                           reconstruct_method;
        riemann_solver_t                               riemann_solver;
        mara::two_body_parameters_t                    binary_params;
        quad_tree_t<location_2d_t>                     vertices;
        quad_tree_t<mara::iso2d::conserved_per_area_t> initial_conserved;
        quad_tree_t<mara::unit_rate<double>>           buffer_rate_field;
    };


    //=========================================================================
    struct solution_t
    {
        mara::unit_time<double>                        time = 0.0;
        mara::rational_number_t                        iteration = 0;
        quad_tree_t<mara::iso2d::conserved_per_area_t> conserved;
        solution_t operator+(const solution_t& other) const;
        solution_t operator*(mara::rational_number_t scale) const;
    };


    //=========================================================================
    struct state_t
    {
        solution_t solution;
        mara::schedule_t schedule;
        mara::config_t run_config;
    };


    //=========================================================================
    struct diagnostic_fields_t
    {
        mara::config_t                                 run_config;
        mara::unit_time<double>                        time;
        quad_tree_t<location_2d_t>                     vertices;
        quad_tree_t<double>                            sigma;
        quad_tree_t<double>                            radial_velocity;
        quad_tree_t<double>                            phi_velocity;
        location_2d_t                                  position_of_mass1;
        location_2d_t                                  position_of_mass2;
    };


    //=========================================================================
    mara::config_template_t      create_config_template();
    mara::config_t               create_run_config   (int argc, const char* argv[]);
    mara::schedule_t             create_schedule     (const mara::config_t& run_config);
    mara::two_body_parameters_t  create_binary_params(const mara::config_t& run_config);
    quad_tree_t<location_2d_t>   create_vertices     (const mara::config_t& run_config);
    solution_t                   create_solution     (const mara::config_t& run_config);
    state_t                      create_state        (const mara::config_t& run_config);
    solver_data_t                create_solver_data  (const mara::config_t& run_config);
    primitive_field_t            create_disk_profile (const mara::config_t& run_config);
    solution_t                   advance(const solution_t& solution, const solver_data_t& solver_data, mara::unit_time<double> dt);
    diagnostic_fields_t          diagnostic_fields(const solution_t& solution, const mara::config_t& run_config);
}




//=============================================================================
namespace mara
{
    template<> void write<binary::solution_t>         (h5::Group&, std::string, const binary::solution_t&);
    template<> void write<binary::state_t>            (h5::Group&, std::string, const binary::state_t&);
    template<> void write<binary::diagnostic_fields_t>(h5::Group&, std::string, const binary::diagnostic_fields_t&);
    template<> void read<binary::solution_t>          (h5::Group&, std::string, binary::solution_t&);
    template<> void read<binary::state_t>             (h5::Group&, std::string, binary::state_t&);
    template<> void read<binary::diagnostic_fields_t> (h5::Group&, std::string, binary::diagnostic_fields_t&);
}
