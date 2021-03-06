/**
 ==============================================================================
 Copyright 2019, Christopher Tiede

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




#pragma once
#include <cmath>
#include "core_geometric.hpp"
#include "core_matrix.hpp"
#include "core_dimensional.hpp"
#include "core_tuple.hpp"
#include "physics_mhd.hpp"





/**
 * @brief      HLLD is implemeted following Miyoshi & Kusano, 2005
 */
struct mara::mhd::riemann_hlld_variables_t 
{
    double gamma_law_index;

    mara::unit_vector_t nhat;
    mara::mhd::primitive_t PL;
    mara::mhd::primitive_t PR;

    mara::arithmetic_sequence_t<double, 3> v_para_l;
    mara::arithmetic_sequence_t<double, 3> v_para_r;
    mara::arithmetic_sequence_t<double, 3> v_perp_l;
    mara::arithmetic_sequence_t<double, 3> v_perp_r;

    mara::arithmetic_sequence_t<double, 3> b_para_l;
    mara::arithmetic_sequence_t<double, 3> b_para_r;
    mara::arithmetic_sequence_t<double, 3> b_perp_l;
    mara::arithmetic_sequence_t<double, 3> b_perp_r;

    double dl;
    double dr;
    double ul;
    double ur;
    double plT;
    double prT;
    double pstar;
    double b_along;

    double SR;
    double SL;
    double SLstar;
    double SRstar;
    double SM;

    auto UL() const { return PL.to_conserved_density(gamma_law_index); }
    auto UR() const { return PR.to_conserved_density(gamma_law_index); }
    auto FL() const { return PL.flux(nhat, gamma_law_index); }
    auto FR() const { return PR.flux(nhat, gamma_law_index); }




    /**
     * @brief      { function_description }
     *
     * @return     { description_of_the_return_value }
     */
    mara::mhd::conserved_density_t UL_star() const
    {
        auto safe_divide = [] (double a, double b, const char* what)
        {
            if (std::abs(b) < 1e-8) throw std::invalid_argument(what); return a / b;
        };


        auto d_star = dl * safe_divide(SL - ul, SL - SM, "UL_star: d_star");
        auto denom  = dl * (SL - ul) * (SL - SM) - b_along * b_along;        
        auto beta    = (SM - ul) / denom;
        auto vx_star =  SM * nhat[0] + v_perp_l[0] - beta * b_along * b_perp_l[0];
        auto vy_star =  SM * nhat[1] + v_perp_l[1] - beta * b_along * b_perp_l[1];
        auto vz_star =  SM * nhat[2] + v_perp_l[2] - beta * b_along * b_perp_l[2];


        auto num     = dl * (SL - ul) * (SL - ul) - b_along * b_along;
        auto zeta    = num / denom;
        auto bx_star = zeta * b_perp_l[0] + b_para_l[0];
        auto by_star = zeta * b_perp_l[1] + b_para_l[1];
        auto bz_star = zeta * b_perp_l[2] + b_para_l[2];


        auto e       = (PL.to_conserved_density(gamma_law_index))[4].value;
        auto bv      =  PL.bfield_dot_velocity();
        auto bv_star =  vx_star * bx_star + vy_star * by_star + vz_star * bz_star;
        auto e_one   = (SL - ul) * e - plT * ul + pstar * SM;
        auto e_star  = safe_divide(e_one + b_along * (bv - bv_star), SL - SM, "mara::mhd::riemann_hlld_variables_t::UL_star (e_star)");


        if (std::abs(denom) < 1e-8)
        {
            return mara::mhd::conserved_density_t{
                dl,
                dl * (SM * nhat[0] + v_perp_l[0]),
                dl * (SM * nhat[1] + v_perp_l[1]),
                dl * (SM * nhat[2] + v_perp_l[2]),
                e_one / (SL - SM),
                b_para_l[0],
                b_para_l[1],
                b_para_l[2],
            };
        }
        return mara::mhd::conserved_density_t{
            d_star,
            d_star * vx_star,
            d_star * vy_star,
            d_star * vz_star,
            e_star ,
            bx_star,
            by_star,
            bz_star
        };
    }




    /**
     * @brief      { function_description }
     *
     * @return     { description_of_the_return_value }
     */
    mara::mhd::conserved_density_t UR_star() const
    {
        auto safe_divide = [] (double a, double b, const char* what)
        {
            if (std::abs(b) < 1e-8) throw std::invalid_argument(what); return a / b;
        };


        auto d_star  = dr * safe_divide(SR - ur, (SR - SM), "UR_star: d_star");
        auto denom   = dr * (SR - ur) * (SR - SM) - b_along * b_along;
        auto beta    = (SM - ur) / denom;
        auto vx_star =  SM * nhat[0] + v_perp_r[0] - beta * b_along * b_perp_r[0];
        auto vy_star =  SM * nhat[1] + v_perp_r[1] - beta * b_along * b_perp_r[1];
        auto vz_star =  SM * nhat[2] + v_perp_r[2] - beta * b_along * b_perp_r[2];


        auto num     = dr * (SR - ur) * (SR - ur) - b_along * b_along;
        auto zeta    = num / denom;
        auto bx_star = zeta * b_perp_r[0] + b_para_r[0];
        auto by_star = zeta * b_perp_r[1] + b_para_r[1];
        auto bz_star = zeta * b_perp_r[2] + b_para_r[2];


        auto e       = (PR.to_conserved_density(gamma_law_index))[4].value;
        auto bv      =  PR.bfield_dot_velocity();
        auto bv_star =  vx_star * bx_star + vy_star * by_star + vz_star * bz_star;
        auto e_one   = (SR - ur) * e - prT * ur + pstar * SM;
        auto e_star  = safe_divide(e_one + b_along * (bv - bv_star), SR - SM, "mara::mhd::riemann_hlld_variables_t::UR_star (e_star)");


        if (std::abs(denom) < 1e-8)
        {
            return mara::mhd::conserved_density_t{
                dr,
                dr * (SM * nhat[0] + v_perp_r[0]),
                dr * (SM * nhat[1] + v_perp_r[1]),
                dr * (SM * nhat[2] + v_perp_r[2]),
                e_one / (SR - SM),
                b_para_r[0],
                b_para_r[1],
                b_para_r[2],
            };
        }
        return mara::mhd::conserved_density_t{
            d_star,
            d_star * vx_star,
            d_star * vy_star,
            d_star * vz_star,
            e_star ,
            bx_star,
            by_star,
            bz_star
        };
    }




    /**
     * @brief      { function_description }
     *
     * @return     { description_of_the_return_value }
     */
    mara::mhd::conserved_density_t UL_starstar() const
    {
        auto sgn = [] (double x) { return std::copysign(1.0, x); };
        auto b_sign  = sgn(b_along);


        if (b_along != 0.0)
        {
            auto Ul_star = UL_star();
            auto d_star  = Ul_star[0].value;
            auto vx_star = Ul_star[1].value / d_star;
            auto vy_star = Ul_star[2].value / d_star;
            auto vz_star = Ul_star[3].value / d_star;
            auto bx_star = Ul_star[5].value;
            auto by_star = Ul_star[6].value;
            auto bz_star = Ul_star[7].value;
            auto bv_star = vx_star * bx_star + vy_star * by_star + vz_star * bz_star;


            auto Ur_star     = UR_star();
            auto v_starstar  = get_v_starstar(Ul_star, Ur_star, b_sign);
            auto b_starstar  = get_b_starstar(Ul_star, Ur_star, b_sign);
            auto vx_starstar = SM * nhat[0] + v_starstar[0] * (1 - nhat[0]);
            auto vy_starstar = SM * nhat[1] + v_starstar[1] * (1 - nhat[1]);
            auto vz_starstar = SM * nhat[2] + v_starstar[2] * (1 - nhat[2]);
            auto bx_starstar = b_para_l[0]  + b_starstar[0] * (1 - nhat[0]);
            auto by_starstar = b_para_l[1]  + b_starstar[1] * (1 - nhat[1]);
            auto bz_starstar = b_para_l[2]  + b_starstar[2] * (1 - nhat[2]);
            auto bv_starstar = vx_starstar * bx_starstar + vy_starstar * by_starstar + vz_starstar * bz_starstar;
            auto e_starstar  = Ul_star[4].value - std::sqrt(d_star) * (bv_star - bv_starstar) * b_sign;


            return mara::mhd::conserved_density_t{
                d_star,
                d_star * (vx_starstar), 
                d_star * (vy_starstar),
                d_star * (vz_starstar),
                e_starstar,
                bx_starstar,
                by_starstar,
                bz_starstar
            };
        }
        return UL_star();
    }




    /**
     * @brief      { function_description }
     *
     * @return     { description_of_the_return_value }
     */
    mara::mhd::conserved_density_t UR_starstar() const
    {
        auto sgn = [] (double x) { return std::copysign(1.0, x); };
        auto b_sign  = sgn(b_along);


        if (b_along != 0.0)
        {
            auto Ur_star = UR_star();
            auto d_star  = Ur_star[0].value;
            auto vx_star = Ur_star[1].value / d_star;
            auto vy_star = Ur_star[2].value / d_star;
            auto vz_star = Ur_star[3].value / d_star;
            auto bx_star = Ur_star[5].value;
            auto by_star = Ur_star[6].value;
            auto bz_star = Ur_star[7].value;
            auto bv_star = vx_star * bx_star + vy_star * by_star + vz_star * bz_star;


            auto Ul_star     = UL_star();
            auto v_starstar  = get_v_starstar(Ul_star, Ur_star, b_sign);
            auto b_starstar  = get_b_starstar(Ul_star, Ur_star, b_sign);
            auto vx_starstar = SM * nhat[0] + v_starstar[0] * (1 - nhat[0]);
            auto vy_starstar = SM * nhat[1] + v_starstar[1] * (1 - nhat[1]);
            auto vz_starstar = SM * nhat[2] + v_starstar[2] * (1 - nhat[2]);
            auto bx_starstar = b_para_r[0]  + b_starstar[0] * (1 - nhat[0]);
            auto by_starstar = b_para_r[1]  + b_starstar[1] * (1 - nhat[1]);
            auto bz_starstar = b_para_r[2]  + b_starstar[2] * (1 - nhat[2]);
            auto bv_starstar = vx_starstar * bx_starstar + vy_starstar * by_starstar + vz_starstar * bz_starstar;
            auto e_starstar  = Ur_star[4].value + std::sqrt(d_star) * (bv_star - bv_starstar) * b_sign;


            return mara::mhd::conserved_density_t{
                d_star,
                d_star * (vx_starstar),
                d_star * (vy_starstar),
                d_star * (vz_starstar),
                e_starstar,
                bx_starstar,
                by_starstar,
                bz_starstar 
            };
        }
        return UR_star();
    }




    /**
     * @brief      Gets the v starstar.
     *
     * @param[in]  UL_star  The ul star
     * @param[in]  UR_star  The ur star
     * @param[in]  b_sign   The b sign
     *
     * @return     The v starstar.
     */
    mara::arithmetic_sequence_t<double,3> get_v_starstar(
        const mara::mhd::conserved_density_t& UL_star,
        const mara::mhd::conserved_density_t& UR_star,
        double b_sign) const
    {
        auto dl_star  = UL_star[0].value;
        auto dr_star  = UR_star[0].value;
        auto ul_star  = UL_star[1].value / dl_star;
        auto ur_star  = UR_star[1].value / dr_star;
        auto vl_star  = UL_star[2].value / dl_star;
        auto vr_star  = UR_star[2].value / dr_star;
        auto wl_star  = UL_star[3].value / dl_star;
        auto wr_star  = UR_star[3].value / dr_star;
        auto bxl_star = UL_star[5].value;
        auto bxr_star = UR_star[5].value;
        auto byl_star = UL_star[6].value;
        auto byr_star = UR_star[6].value;
        auto bzl_star = UL_star[7].value;
        auto bzr_star = UR_star[7].value;


        auto rt_dl_star = std::sqrt(dl_star);
        auto rt_dr_star = std::sqrt(dr_star);


        auto eta        =  rt_dl_star + rt_dr_star;
        auto u_starstar = (rt_dl_star * ul_star + rt_dr_star * ur_star + (bxr_star - bxl_star) * b_sign) / eta;
        auto v_starstar = (rt_dl_star * vl_star + rt_dr_star * vr_star + (byr_star - byl_star) * b_sign) / eta;
        auto w_starstar = (rt_dl_star * wl_star + rt_dr_star * wr_star + (bzr_star - bzl_star) * b_sign) / eta;


        return mara::arithmetic_sequence_t<double, 3>{
            u_starstar,
            v_starstar,
            w_starstar
        };
    }




    /**
     * @brief      Gets the b starstar.
     *
     * @param[in]  UL_star  The ul star
     * @param[in]  UR_star  The ur star
     * @param[in]  b_sign   The b sign
     *
     * @return     The b starstar.
     */
    mara::arithmetic_sequence_t<double,3> get_b_starstar(
        const conserved_density_t& UL_star,
        const conserved_density_t& UR_star,
        double b_sign) const
    {
        auto dl_star  = UL_star[0].value;
        auto dr_star  = UR_star[0].value;
        auto ul_star  = UL_star[1].value / dl_star;
        auto ur_star  = UR_star[1].value / dr_star;
        auto vl_star  = UL_star[2].value / dl_star;
        auto vr_star  = UR_star[2].value / dr_star;
        auto wl_star  = UL_star[3].value / dl_star;
        auto wr_star  = UR_star[3].value / dr_star;
        auto bxl_star = UL_star[5].value;
        auto bxr_star = UR_star[5].value;
        auto byl_star = UL_star[6].value;
        auto byr_star = UR_star[6].value;
        auto bzl_star = UL_star[7].value;
        auto bzr_star = UR_star[7].value;


        auto rt_dl_star = std::sqrt(dl_star);
        auto rt_dr_star = std::sqrt(dr_star);


        auto eta         = rt_dl_star + rt_dr_star;
        auto bx_starstar = rt_dl_star * bxr_star + rt_dr_star * bxl_star + rt_dl_star * rt_dr_star * (ur_star - ul_star) * b_sign;
        auto by_starstar = rt_dl_star * byr_star + rt_dr_star * byl_star + rt_dl_star * rt_dr_star * (vr_star - vl_star) * b_sign;
        auto bz_starstar = rt_dl_star * bzr_star + rt_dr_star * bzl_star + rt_dl_star * rt_dr_star * (wr_star - wl_star) * b_sign;


        return mara::arithmetic_sequence_t<double, 3>{
            bx_starstar / eta,
            by_starstar / eta,
            bz_starstar / eta
        };
    }


    auto FL_star()     const { return FL() + (UL_star() - UL()) * make_velocity(SL); }
    auto FR_star()     const { return FR() + (UR_star() - UR()) * make_velocity(SR); }
    auto FL_starstar() const { return FL() + (UL_star() - UL()) * make_velocity(SL) + (UL_starstar() - UL_star()) * make_velocity(SLstar); }
    auto FR_starstar() const { return FR() + (UR_star() - UR()) * make_velocity(SR) + (UR_starstar() - UR_star()) * make_velocity(SRstar); }


    /**
     * @brief      { function_description }
     *
     * @return     { description_of_the_return_value }
     */
    mara::mhd::flux_vector_t interface_flux() const
    {
        if      (0.0     <= SL                  ) return FL();
        else if (SL      <= 0.0 && 0.0 <= SLstar) return FL_star();
        else if (SLstar  <= 0.0 && 0.0 <= SM    ) return FL_starstar();
        else if (SM      <= 0.0 && 0.0 <= SRstar) return FR_starstar();
        else if (SRstar  <= 0.0 && 0.0 <= SR    ) return FR_star();
        else if (SR      <= 0.0                 ) return FR();
        throw std::invalid_argument("riemann_hlld_variables_t::interface_flux (unordered wave speeds)");
    }
};




/**
 * @brief      Computer variables required to solve the HLLD jump conditions
 *
 * @param[in]  Pl               The state to the left of the interface
 * @param[in]  Pr               The state to the right
 * @param[in]  nhat             The normal vector to the interface
 * @param[in]  gamma_law_index  The gamma law index
 *
 * @return     The HLLD variables struct
 */
inline mara::mhd::riemann_hlld_variables_t mara::mhd::compute_hlld_variables(
    const mara::mhd::primitive_t& Pl,
    const mara::mhd::primitive_t& Pr,
    const mara::unit_vector_t& nhat,
    double gamma_law_index)
{
    auto throw_if_nan = [] (double x, const char* what)
    {
        if (std::isnan(x))
            throw std::invalid_argument(what);

        return x;
    };
    auto safe_divide = [] (double a, double b, const char* what)
    {
        if (std::abs(b) < 1e-8) throw std::invalid_argument(what); return a / b;
    };


    // Left and Right prims for calculations
    // ========================================================================
    auto dr  = throw_if_nan(Pr.mass_density()      , "mara::mhd::compute_hlld_variables (nan density_r)");
    auto dl  = throw_if_nan(Pl.mass_density()      , "mara::mhd::compute_hlld_variables (nan density_l)");
    auto ur  = throw_if_nan(Pr.velocity_along(nhat), "mara::mhd::compute_hlld_variables (nan velocity_r)");
    auto ul  = throw_if_nan(Pl.velocity_along(nhat), "mara::mhd::compute_hlld_variables (nan velocity_l)");
    auto prT = Pr.gas_pressure() + 0.5 * Pr.bfield_squared();
    auto plT = Pl.gas_pressure() + 0.5 * Pl.bfield_squared();


    // Left and Right parallel and perpendicular velocity and field vectors
    // ========================================================================
    auto b_along  = Pl.bfield_along(nhat);
    auto v_para_l = nhat * ul;
    auto v_para_r = nhat * ur;
    auto b_para_l = nhat * b_along;
    auto b_para_r = nhat * b_along;
    auto v_perp_l = Pl.velocity() - v_para_l;
    auto v_perp_r = Pr.velocity() - v_para_r;
    auto b_perp_l = Pl.bfield()   - b_para_l;
    auto b_perp_r = Pr.bfield()   - b_para_r;


    // Get fast waves and the outermost signal speeds
    // ========================================================================
    auto cfl = std::sqrt(throw_if_nan(Pl.magnetosonic_speed_squared_fast(nhat, gamma_law_index), "mara::mhd::compute_hlld_variables (nan magnetoacoustic speed)"));
    auto cfr = std::sqrt(throw_if_nan(Pr.magnetosonic_speed_squared_fast(nhat, gamma_law_index), "mara::mhd::compute_hlld_variables (nan magnetoacoustic speed)"));
    auto SL  = std::min(ul, ur) - std::max(cfl, cfr);
    auto SR  = std::max(ul, ur) + std::max(cfl, cfr);
    

    // Get signal speed associated with the entropy wave: Eq.(38)
    // ========================================================================
    auto SM_top = (SR - ur) * dr * ur - (SL - ul) * dl * ul - prT + plT; 
    auto SM_bot = (SR - ur) * dr - (SL - ul) * dl;
    auto SM     = safe_divide(SM_top, SM_bot, "mara::mhd::compute_hlld_variables (SM)");


    // Get signal speed associated with internal alfven waves: Eqs.(43) & (51)
    // ========================================================================
    auto dstar_l = dl * safe_divide(SL - ul, SL - SM, "mara::mhd::compute_hlld_variables (dstar_l)");
    auto dstar_r = dr * safe_divide(SR - ur, SR - SM, "mara::mhd::compute_hlld_variables (dstar_r)");
    auto SLstar  = SM - safe_divide(std::abs(b_along), std::sqrt(dstar_l), "mara::mhd::compute_hlld_variables (SLstar)");
    auto SRstar  = SM + safe_divide(std::abs(b_along), std::sqrt(dstar_r), "mara::mhd::compute_hlld_variables (SRstar)");


    // Get the average-total-pressure in Riemann fan: Eq.(41)
    // ========================================================================
    auto pstar_one = (SR - ur) * dr * plT - (SL - ul) * dl * prT;
    auto pstar_two = (SR - ur) * (SL - ul) * (ur - ul) * dl * dr;
    auto pstar_bot = (SR - ur) * dr - (SL - ul) * dl;
    auto pstar = safe_divide(pstar_one + pstar_two, pstar_bot, "mara::mhd::compute_hlld_variables (pT_star)");


    // Validate a bunch of things
    // ========================================================================
    if (Pr.bfield_along(nhat) != Pl.bfield_along(nhat))
        throw std::invalid_argument("mara::mhd::compute_hlld_variables (jump in longitudinal B-field)");

    if (std::isnan(SL) || std::isnan(SR))
        throw std::invalid_argument("mara::mhd::compute_hlld_variables (nan left or right wave speeds)");

    if (std::isnan(SLstar) || std::isnan(SRstar))
        throw std::invalid_argument("mara::mhd::compute_hlld_variables (nan intermediate wavespeed)");

    if (std::isnan(SM))
        throw std::invalid_argument("mara::mhd::compute_hlld_variables (nan contact speed)");

    if (dl <= 0.0 || dr <= 0.0)
        throw std::invalid_argument("mara::mhd::compute_hlld_variables (negative density)");

    if (dstar_l < 0.0 || dstar_r < 0.0)
        throw std::invalid_argument("mara::mhd::compute_hlld_variables (negative intermediate density)");


    // Pack the results into a struct and return it
    // ========================================================================
    auto r     = riemann_hlld_variables_t();
    r.nhat     = nhat;
    r.PL       = Pl;
    r.PR       = Pr;
    r.v_para_l = v_para_l;
    r.v_para_r = v_para_r;
    r.v_perp_l = v_perp_l;
    r.v_perp_r = v_perp_r;
    r.b_para_l = b_para_l;
    r.b_para_r = b_para_r;
    r.b_perp_l = b_perp_l;
    r.b_perp_r = b_perp_r;
    r.dl       = dl;
    r.dr       = dr;
    r.ul       = ul;
    r.ur       = ur;
    r.prT      = prT;
    r.plT      = plT;
    r.pstar    = pstar;
    r.b_along  = b_along;
    r.SL       = SL;
    r.SR       = SR;
    r.SLstar   = SLstar;
    r.SRstar   = SRstar;
    r.SM       = SM;
    r.gamma_law_index = gamma_law_index;

    return r;
}




/**
 * @brief      Return the HLLD flux for the given pair of states
 *
 * @param[in]  Pl               The state to the left of the interface
 * @param[in]  Pr               The state to the right
 * @param[in]  b_para           The component of the magnetic field parallel to nhat
 * @param[in]  nhat             The normal vector to the interface
 * @param[in]  gamma_law_index  The gamma law index
 *
 * @return     A vector of fluxes
 */
inline mara::mhd::flux_vector_t mara::mhd::riemann_hlld(
        const primitive_t& Pl,
        const primitive_t& Pr,
        const unit_field& b_along,
        const unit_vector_t& nhat,
        double gamma_law_index)
{
    auto Plp = Pl.with_b_along(b_along, nhat);
    auto Prp = Pr.with_b_along(b_along, nhat);
    return compute_hlld_variables(Plp, Prp, nhat, gamma_law_index).interface_flux();
}
