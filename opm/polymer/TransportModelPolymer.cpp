/*
  Copyright 2012 SINTEF ICT, Applied Mathematics.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <opm/polymer/TransportModelPolymer.hpp>
#include <opm/core/fluid/IncompPropertiesInterface.hpp>
#include <opm/core/grid.h>
#include <opm/core/utility/RootFinders.hpp>
#include <cmath>

static double norm(double* res);


namespace Opm
{
    TransportModelPolymer::TransportModelPolymer(const UnstructuredGrid& grid,
						 const double* porosity,
						 const double* porevolume,
						 const IncompPropertiesInterface& props,
						 const PolymerData& polyprops,
						 const int method,
						 const double tol,
						 const int maxit)
	: grid_(grid),
	  porosity_(porosity),
	  porevolume_(porevolume),
	  props_(props),
	  polyprops_(polyprops),
	  tol_(tol),
	  maxit_(maxit),
	  darcyflux_(0),
	  source_(0),
	  dt_(0.0),
	  inflow_c_(0.0),
	  saturation_(0),
	  concentration_(0),
	  cmax_(0),
	  fractionalflow_(grid.number_of_cells, -1.0),
	  mc_(grid.number_of_cells, -1.0),
	  method_(method)
    {
	if (props.numPhases() != 2) {
	    THROW("Property object must have 2 phases");
	}
	visc_ = props.viscosity();

	// Set up smin_ and smax_
	int num_cells = props.numCells();
	smin_.resize(props.numPhases()*num_cells);
	smax_.resize(props.numPhases()*num_cells);
	std::vector<int> cells(num_cells);
	for (int i = 0; i < num_cells; ++i) {
	    cells[i] = i;
	}
	props.satRange(props.numCells(), &cells[0], &smin_[0], &smax_[0]);
    }




    void TransportModelPolymer::solve(const double* darcyflux,
				      const double* source,
				      const double dt,
				      const double inflow_c,
				      double* saturation,
				      double* concentration,
				      double* cmax)
    {
	darcyflux_ = darcyflux;
	source_ = source;
	dt_ = dt;
	inflow_c_ = inflow_c;
	saturation_ = saturation;
	concentration_ = concentration;
	cmax_ = cmax;
	reorderAndTransport(grid_, darcyflux);
    }




    // Residual for saturation equation, single-cell implicit Euler transport
    //
    //     r(s) = s - s0 + dt/pv*( influx + outflux*f(s) )
    //
    // where influx is water influx, outflux is total outflux.
    // Influxes are negative, outfluxes positive.
    struct TransportModelPolymer::ResidualS
    {
	const TransportModelPolymer& tm_;
	const int cell_;
	const double s0_;
	const double influx_;  // sum_j min(v_ij, 0)*f(s_j)
	const double outflux_; // sum_j max(v_ij, 0)
	const double dtpv_;    // dt/pv(i)
	const double c_;
	explicit ResidualS(const TransportModelPolymer& tmodel,
			   const int cell,
			   const double s0,
			   const double influx,
			   const double outflux,
			   const double dtpv,
			   const double c)
	    : tm_(tmodel),
	      cell_(cell),
	      s0_(s0),
	      influx_(influx),
	      outflux_(outflux),
	      dtpv_(dtpv),
	      c_(c)
	{
	}
	double operator()(double s) const
	{
	    return s - s0_ +  dtpv_*(outflux_*tm_.fracFlow(s, c_, cell_) + influx_);
	}
    };



    // Residual for concentration equation, single-cell implicit Euler transport
    //
    //  \TODO doc me
    // where ...
    // Influxes are negative, outfluxes positive.
    struct TransportModelPolymer::ResidualC
    {
	int cell;
	double s0;
	double c0;
	double cmax0;
	double influx;  // sum_j min(v_ij, 0)*f(s_j)
	double influx_polymer;  // sum_j min(v_ij, 0)*f(s_j)*mc(c_j)
	double outflux; // sum_j max(v_ij, 0)
	double porosity;
	double dtpv;    // dt/pv(i)
	mutable double s; // Mutable in order to change it with every operator() call to be the last computed s value.
	const TransportModelPolymer& tm;
	explicit ResidualC(const TransportModelPolymer& tmodel, int cell_index)
	    : tm(tmodel)
	{
	    cell    = cell_index;
	    s0      = tm.saturation_[cell];
	    c0      = tm.concentration_[cell];
	    cmax0   = tm.cmax_[cell];
            double dflux       = -tm.source_[cell];
            bool src_is_inflow = dflux < 0.0;
	    influx  =  src_is_inflow ? dflux : 0.0;
	    influx_polymer = src_is_inflow ? dflux*tm.computeMc(tm.inflow_c_) : 0.0;
	    outflux = !src_is_inflow ? dflux : 0.0;
	    dtpv    = tm.dt_/tm.porevolume_[cell];
	    porosity = tm.porosity_[cell];
	    s = -1e100;

	    for (int i = tm.grid_.cell_facepos[cell]; i < tm.grid_.cell_facepos[cell+1]; ++i) {
		int f = tm.grid_.cell_faces[i];
		double flux;
		int other;
		// Compute cell flux
		if (cell == tm.grid_.face_cells[2*f]) {
		    flux  = tm.darcyflux_[f];
		    other = tm.grid_.face_cells[2*f+1];
		} else {
		    flux  =-tm.darcyflux_[f];
		    other = tm.grid_.face_cells[2*f];
		}
		// Add flux to influx or outflux, if interior.
		if (other != -1) {
		    if (flux < 0.0) {
			influx  += flux*tm.fractionalflow_[other];
			influx_polymer += flux*tm.fractionalflow_[other]*tm.mc_[other];
		    } else {
			outflux += flux;
		    }
		}
	    }
	}
	double operator()(double c) const
	{
	    ResidualS res_s(tm, cell, s0, influx, outflux, dtpv, c);
	    int iters_used;
	    // Solve for s first.
	    s = modifiedRegulaFalsi(res_s, tm.smin_[2*cell], tm.smax_[2*cell], tm.maxit_, tm.tol_, iters_used);
	    double ff = tm.fracFlow(s, c, cell);
	    double mc = tm.computeMc(c);
	    double dps = tm.polyprops_.dps;
	    double rhor = tm.polyprops_.rhor;
	    double ads0 = tm.polyprops_.adsorbtion(std::max(c0, cmax0));
	    double ads = tm.polyprops_.adsorbtion(std::max(c, cmax0));
	    double res = (s - dps)*c - (s0 - dps)*c0
		+ rhor*((1.0 - porosity)/porosity)*(ads - ads0)
		+ dtpv*(outflux*ff*mc + influx_polymer);
#ifdef EXTRA_DEBUG_OUTPUT
	    std::cout << "c = " << c << "    s = " << s << "    c-residual = " << res << std::endl;
#endif
	    return res;
	}

	double lastSaturation() const
	{
	    return s;
	}
    };


    // Residual for s and c. Includes method to compute the gradient.
    struct TransportModelPolymer::Residual
    {
	int cell;
	double s0;
	double c0;
	double cmax0;
	double influx;  // sum_j min(v_ij, 0)*f(s_j)
	double influx_polymer;  // sum_j min(v_ij, 0)*f(s_j)*mc(c_j)
	double outflux; // sum_j max(v_ij, 0)
	double porosity;
	double dtpv;    // dt/pv(i)
	const TransportModelPolymer& tm;

	Residual(const TransportModelPolymer& tmodel, int cell_index)
	    : tm(tmodel)
	{
	    cell    = cell_index;
	    s0      = tm.saturation_[cell];
	    c0      = tm.concentration_[cell];
	    cmax0   = tm.cmax_[cell];
            double dflux       = -tm.source_[cell];
            bool src_is_inflow = dflux < 0.0;
	    influx  =  src_is_inflow ? dflux : 0.0;
	    influx_polymer = src_is_inflow ? dflux*tm.computeMc(tm.inflow_c_) : 0.0;
	    outflux = !src_is_inflow ? dflux : 0.0;
	    dtpv    = tm.dt_/tm.porevolume_[cell];
	    porosity = tm.porosity_[cell];

	    for (int i = tm.grid_.cell_facepos[cell]; i < tm.grid_.cell_facepos[cell+1]; ++i) {
		int f = tm.grid_.cell_faces[i];
		double flux;
		int other;
		// Compute cell flux
		if (cell == tm.grid_.face_cells[2*f]) {
		    flux  = tm.darcyflux_[f];
		    other = tm.grid_.face_cells[2*f+1];
		} else {
		    flux  =-tm.darcyflux_[f];
		    other = tm.grid_.face_cells[2*f];
		}
		// Add flux to influx or outflux, if interior.
		if (other != -1) {
		    if (flux < 0.0) {
			influx  += flux*tm.fractionalflow_[other];
			influx_polymer += flux*tm.fractionalflow_[other]*tm.mc_[other];
		    } else {
			outflux += flux;
		    }
		}
	    }
	}

	void computeResidual(const double* x, double* res) const
	{
	    double s = x[0];
	    double c = x[1];
	    double ff = tm.fracFlow(s, c, cell);
	    double mc = tm.computeMc(c);
	    double dps = tm.polyprops_.dps;
	    double rhor = tm.polyprops_.rhor;
	    double ads0 = tm.polyprops_.adsorbtion(std::max(c0, cmax0));
	    double ads = tm.polyprops_.adsorbtion(std::max(c, cmax0));
	    res[0] = s - s0 +  dtpv*(outflux*tm.fracFlow(s, c, cell) + influx);
	    res[1] = (s - dps)*c - (s0 - dps)*c0
		+ rhor*((1.0 - porosity)/porosity)*(ads - ads0)
		+ dtpv*(outflux*ff*mc + influx_polymer);
	}

	// Compute gradient using finite difference.
	void computeGradient(const double* x, double* res, double* gradient, bool if_res_s, const int& method) const
     	// If if_res_s == true, compute the gradient of s-residual, otherwise, compute gradient of c-residual.
	// If method == 1, use finite diference
	// If method == 2, use analytic expresions
	{
	    if (method == 1) {
		    double epsi = 1e-5;
		    double res_epsi[2];
		    double x_epsi[2];
		    computeResidual(x, res);
		    if (if_res_s) {
			x_epsi[0] = x[0] + epsi;
			x_epsi[1] = x[1];
			computeResidual(x_epsi, res_epsi);
			gradient[0] =  (res_epsi[0] - res[0])/epsi;
			x_epsi[0] = x[0];
			x_epsi[1] = x[1] + epsi;
			computeResidual(x_epsi, res_epsi);
			gradient[1] = (res_epsi[0] - res[0])/epsi;
		    } else {
			x_epsi[0] = x[0] + epsi;
			x_epsi[1] = x[1];
			computeResidual(x_epsi, res_epsi);
			gradient[0] =  (res_epsi[1] - res[1])/epsi;
			x_epsi[0] = x[0];
			x_epsi[1] = x[1] + epsi;
			computeResidual(x_epsi, res_epsi);
			gradient[1] = (res_epsi[1] - res[1])/epsi;
		    }
		} else if (method == 2)  {
		    double s = x[0];
		    double c = x[1];
		    double ff_ds_dc[2];
		    double ff = tm.fracFlowWithDer(s, c, cell, ff_ds_dc);
		    double mc_dc;
		    double mc = tm.computeMcWithDer(c, &mc_dc);
		    double dps = tm.polyprops_.dps;
		    double rhor = tm.polyprops_.rhor;
		    double ads0 = tm.polyprops_.adsorbtion(std::max(c0, cmax0));
		    double ads;
		    double ads_dc;
		    if (c < cmax0) {
			ads = tm.polyprops_.adsorbtion(cmax0);
			ads_dc = 0;
		    } else {
			ads = tm.polyprops_.adsorbtionWithDer(c, &ads_dc);
		    }
		    res[0] = s - s0 +  dtpv*(outflux*tm.fracFlow(s, c, cell) + influx);
		    res[1] = (s - dps)*c - (s0 - dps)*c0
			+ rhor*((1.0 - porosity)/porosity)*(ads - ads0)
			+ dtpv*(outflux*ff*mc + influx_polymer);
		    if (if_res_s == true) {
			gradient[0] = 1 + dtpv*outflux*ff_ds_dc[0];
			gradient[1] = dtpv*outflux*ff_ds_dc[1];
		    } else if (if_res_s == false) {
			gradient[0] = c + dtpv*outflux*(ff_ds_dc[0])*mc;
			gradient[1] = s - dps + rhor*((1.0 - porosity)/porosity)*(ads_dc - ads0)
			    + dtpv*outflux*(ff_ds_dc[1]*mc + ff*mc_dc);
		    }
		}

	}

    };

    // Compute residual in s for a given piecewise linear curve (with only one node) in the s-c
    // plane. The method operator() is used by a 1d root solver.

    struct TransportModelPolymer::ResidualSDir
    {
	int cell;
	double s0;
	double c0;
	double cmax0;
	double influx;  // sum_j min(v_ij, 0)*f(s_j)
	double influx_polymer;  // sum_j min(v_ij, 0)*f(s_j)*mc(c_j)
	double outflux; // sum_j max(v_ij, 0)
	double porosity;
	double dtpv;    // dt/pv(i)
	double direction[2];
	double end_point[2];
	double x_max[2];
	double x_min[2];
	double t_out;
	double t_max; // t_max = t_out + 1
	double x_out[2];
	double x[2];
	const TransportModelPolymer& tm;

	ResidualSDir(const TransportModelPolymer& tmodel, int cell_index)
	    : tm(tmodel)
	{
	    cell    = cell_index;
	    s0      = tm.saturation_[cell];
	    c0      = tm.concentration_[cell];
	    cmax0   = tm.cmax_[cell];
            double dflux       = -tm.source_[cell];
            bool src_is_inflow = dflux < 0.0;
	    influx  =  src_is_inflow ? dflux : 0.0;
	    influx_polymer = src_is_inflow ? dflux*tm.computeMc(tm.inflow_c_) : 0.0;
	    outflux = !src_is_inflow ? dflux : 0.0;
	    dtpv    = tm.dt_/tm.porevolume_[cell];
	    porosity = tm.porosity_[cell];

	    for (int i = tm.grid_.cell_facepos[cell]; i < tm.grid_.cell_facepos[cell+1]; ++i) {
		int f = tm.grid_.cell_faces[i];
		double flux;
		int other;
		// Compute cell flux
		if (cell == tm.grid_.face_cells[2*f]) {
		    flux  = tm.darcyflux_[f];
		    other = tm.grid_.face_cells[2*f+1];
		} else {
		    flux  =-tm.darcyflux_[f];
		    other = tm.grid_.face_cells[2*f];
		}
		// Add flux to influx or outflux, if interior.
		if (other != -1) {
		    if (flux < 0.0) {
			influx  += flux*tm.fractionalflow_[other];
			influx_polymer += flux*tm.fractionalflow_[other]*tm.mc_[other];
		    } else {
			outflux += flux;
		    }
		}
	    }
	}


	// For a given point x=(s,c) in the s,c plane, set up a piecewise linear curve wich starts
	// from "x" with slope "direction", hits the bound of the rectangle
	// [s_min,s_max]x[c_min,c_max] and continue in a straight line to "end_point".  The curve is
	// parametrized by t in [0, t_max], t_out is equal to t when the curve hits the bounding
	// rectangle, x_out=(s_out, c_out) denotes the values of s and c at that point.

	void setup(const double* x_arg, const double* direction_arg, const double* end_point_arg, const double* x_min_arg, const double* x_max_arg, double& t_max_out, double& t_out_out)
	{
	    double t0, t1;
	    x[0] = x_arg[0];
	    x[1] = x_arg[1];
	    x_max[0] = x_max_arg[0];
	    x_max[1] = x_max_arg[1];
	    x_min[0] = x_min_arg[0];
	    x_min[1] = x_min_arg[1];
	    direction[0] = direction_arg[0];
	    direction[1] = direction_arg[1];
	    end_point[0] = end_point_arg[0];
	    end_point[1] = end_point_arg[1];
	    if ((end_point[0]-x[0])*direction[0] + (end_point[1]-x[1])*direction[1] < 0) {
		direction[0] *= -1;
		direction[1] *= -1;
	    }
	    if (direction[0] > 0) {
		t0 = (x_max[0] - x[0])/direction[0];
	    } else {
		t0 = (x_min[0] - x[0])/direction[0];
	    }
	    if (direction[1] > 0) {
		t1 = (x_max[1] - x[1])/direction[1];
	    } else {
		t1 = (x_min[1] - x[1])/direction[1];
	    }
	    t_out = std::min(t0, t1);
	    x_out[0] = x[0] + t_out*direction[0];
	    x_out[1] = x[1] + t_out*direction[1];
	    t_max = t_out + 1;
	    t_max_out = t_max;
	    t_out_out = t_out;
	}


	// Compute x=(s,c) for a given t (t is the parameter for the piecewise linear curve)
	void compute_new_x(double* x_new, const double t) {
	    if (t <= t_out) {
		x_new[0] = x[0] + t*direction[0];
		x_new[1] = x[1] + t*direction[1];
	    } else {
		x_new[0] = 1/(t_max-t_out)*((t_max - t)*x_out[0] + end_point[0]*(t - t_out));
		x_new[1] = 1/(t_max-t_out)*((t_max - t)*x_out[1] + end_point[1]*(t - t_out));
	    }
	}



	double operator()(double t) const
	{
	    double s;
	    double c;
	    if (t <= t_out) {
		s = x[0] + t*direction[0];
		c = x[1] + t*direction[1];
	    } else {
		s = 1/(t_max-t_out)*((t_max - t)*x_out[0] + end_point[0]*(t - t_out));
		c = 1/(t_max-t_out)*((t_max - t)*x_out[1] + end_point[1]*(t - t_out));
	    }
	    return s - s0 +  dtpv*(outflux*tm.fracFlow(s, c, cell) + influx);
	}


    };

    // Same as ResidualSDir but for the residual in c
    struct TransportModelPolymer::ResidualCDir
    {
	int cell;
	double s0;
	double c0;
	double cmax0;
	double influx;  // sum_j min(v_ij, 0)*f(s_j)
	double influx_polymer;  // sum_j min(v_ij, 0)*f(s_j)*mc(c_j)
	double outflux; // sum_j max(v_ij, 0)
	double porosity;
	double dtpv;    // dt/pv(i)
	double direction[2];
	double end_point[2];
	double t_out;
	double t_max; // t_max = t_out + 1
	double x_out[2];
	double x_min[2];
	double x_max[2];
	double x[2];
	const TransportModelPolymer& tm;

	ResidualCDir(const TransportModelPolymer& tmodel, int cell_index)
	    : tm(tmodel)
	{
	    cell    = cell_index;
	    s0      = tm.saturation_[cell];
	    c0      = tm.concentration_[cell];
	    cmax0   = tm.cmax_[cell];
            double dflux       = -tm.source_[cell];
            bool src_is_inflow = dflux < 0.0;
	    influx  =  src_is_inflow ? dflux : 0.0;
	    influx_polymer = src_is_inflow ? dflux*tm.computeMc(tm.inflow_c_) : 0.0;
	    outflux = !src_is_inflow ? dflux : 0.0;
	    dtpv    = tm.dt_/tm.porevolume_[cell];
	    porosity = tm.porosity_[cell];

	    for (int i = tm.grid_.cell_facepos[cell]; i < tm.grid_.cell_facepos[cell+1]; ++i) {
		int f = tm.grid_.cell_faces[i];
		double flux;
		int other;
		// Compute cell flux
		if (cell == tm.grid_.face_cells[2*f]) {
		    flux  = tm.darcyflux_[f];
		    other = tm.grid_.face_cells[2*f+1];
		} else {
		    flux  =-tm.darcyflux_[f];
		    other = tm.grid_.face_cells[2*f];
		}
		// Add flux to influx or outflux, if interior.
		if (other != -1) {
		    if (flux < 0.0) {
			influx  += flux*tm.fractionalflow_[other];
			influx_polymer += flux*tm.fractionalflow_[other]*tm.mc_[other];
		    } else {
			outflux += flux;
		    }
		}
	    }
	}

	void compute_new_x(double* x_new, const double t) {
	    if (t <= t_out) {
		x_new[0] = x[0] + t*direction[0];
		x_new[1] = x[1] + t*direction[1];
	    } else {
		x_new[0] = 1/(t_max-t_out)*((t_max - t)*x_out[0] + end_point[0]*(t - t_out));
		x_new[1] = 1/(t_max-t_out)*((t_max - t)*x_out[1] + end_point[1]*(t - t_out));
	    }
	}

	void setup(const double* x_arg, const double* direction_arg, const double* end_point_arg, const double* x_min_arg, const double* x_max_arg, double& t_max_out, double& t_out_out)
	{
	    bool if_t0 = true;
	    bool if_t1 = true;
	    double t0, t1;
	    x[0] = x_arg[0];
	    x[1] = x_arg[1];
	    x_max[0] = x_max_arg[0];
	    x_max[1] = x_max_arg[1];
	    x_min[0] = x_min_arg[0];
	    x_min[1] = x_min_arg[1];
	    direction[0] = direction_arg[0];
	    direction[1] = direction_arg[1];
	    end_point[0] = end_point_arg[0];
	    end_point[1] = end_point_arg[1];
	    if ((end_point[0]-x[0])*direction[0] + (end_point[1]-x[1])*direction[1] < 0) {
		direction[0] *= -1;
		direction[1] *= -1;
	    }
	    if (direction[0] == 0) {
		if_t0 = false;
	    } else {
		if (direction[0] > 0) {
		    t0 = (x_max[0] - x[0])/direction[0];
		} else {
		    t0 = (x_min[0] - x[0])/direction[0];
		}
	    }
	    if (direction[1] == 0) {
		if_t1 = false;
	    } else {
		if (direction[1] > 0) {
		    t1 = (x_max[1] - x[1])/direction[1];
		} else {
		    t1 = (x_min[1] - x[1])/direction[1];
		}
	    }
	    if (if_t0 && if_t1) {
		t_out = std::min(t0, t1);
	    } else {
		if (if_t0) {
		    t_out = t0;
		} else {
		    t_out = t1;
		}
	    }
	    x_out[0] = x[0] + t_out*direction[0];
	    x_out[1] = x[1] + t_out*direction[1];
	    t_max = t_out + 1;
	    t_max_out = t_max;
	    t_out_out = t_out;
	}

	double operator()(double t) const
	{
	    double s;
	    double c;
	    if (t <= t_out) {
		s = x[0] + t*direction[0];
		c = x[1] + t*direction[1];
	    } else {
		s = 1/(t_max-t_out)*((t_max - t)*x_out[0] + end_point[0]*(t - t_out));
		c = 1/(t_max-t_out)*((t_max - t)*x_out[1] + end_point[1]*(t - t_out));
	    }
	    double ff = tm.fracFlow(s, c, cell);
	    double mc = tm.computeMc(c);
	    double dps = tm.polyprops_.dps;
	    double rhor = tm.polyprops_.rhor;
	    double ads0 = tm.polyprops_.adsorbtion(std::max(c0, cmax0));
	    double ads = tm.polyprops_.adsorbtion(std::max(c, cmax0));
	    return (s - dps)*c - (s0 - dps)*c0
		+ rhor*((1.0 - porosity)/porosity)*(ads - ads0)
		+ dtpv*(outflux*ff*mc + influx_polymer);
	}
    };

    struct TransportModelPolymer::ResidualDir
    {
	int cell;
	int s_or_c; // s_or_c = 0 if s direction, s_or_c = 1 if c direction,
	double s0;
	double c0;
	double cmax0;
	double influx;  // sum_j min(v_ij, 0)*f(s_j)
	double influx_polymer;  // sum_j min(v_ij, 0)*f(s_j)*mc(c_j)
	double outflux; // sum_j max(v_ij, 0)
	double porosity;
	double dtpv;    // dt/pv(i)
	double direction[2];
	double end_point[2];
	double x_max[2];
	double x_min[2];
	double t_out;
	double t_max; // t_max = t_out + 1
	double x_out[2];
	double x[2];
	const TransportModelPolymer& tm;

	ResidualDir(const TransportModelPolymer& tmodel, int cell_index)
	    : tm(tmodel)
	{
	    cell    = cell_index;
	    s0      = tm.saturation_[cell];
	    c0      = tm.concentration_[cell];
	    cmax0   = tm.cmax_[cell];
            double dflux       = -tm.source_[cell];
            bool src_is_inflow = dflux < 0.0;
	    influx  =  src_is_inflow ? dflux : 0.0;
	    influx_polymer = src_is_inflow ? dflux*tm.computeMc(tm.inflow_c_) : 0.0;
	    outflux = !src_is_inflow ? dflux : 0.0;
	    dtpv    = tm.dt_/tm.porevolume_[cell];
	    porosity = tm.porosity_[cell];

	    for (int i = tm.grid_.cell_facepos[cell]; i < tm.grid_.cell_facepos[cell+1]; ++i) {
		int f = tm.grid_.cell_faces[i];
		double flux;
		int other;
		// Compute cell flux
		if (cell == tm.grid_.face_cells[2*f]) {
		    flux  = tm.darcyflux_[f];
		    other = tm.grid_.face_cells[2*f+1];
		} else {
		    flux  =-tm.darcyflux_[f];
		    other = tm.grid_.face_cells[2*f];
		}
		// Add flux to influx or outflux, if interior.
		if (other != -1) {
		    if (flux < 0.0) {
			influx  += flux*tm.fractionalflow_[other];
			influx_polymer += flux*tm.fractionalflow_[other]*tm.mc_[other];
		    } else {
			outflux += flux;
		    }
		}
	    }
	}


	// For a given point x=(s,c) in the s,c plane, set up a piecewise linear curve wich starts
	// from "x" with slope "direction", hits the bound of the rectangle
	// [s_min,s_max]x[c_min,c_max] and continue in a straight line to "end_point".  The curve is
	// parametrized by t in [0, t_max], t_out is equal to t when the curve hits the bounding
	// rectangle, x_out=(s_out, c_out) denotes the values of s and c at that point.

	void setup(const double* x_arg, const double* direction_arg, const double* end_point_arg, const double* x_min_arg, const double* x_max_arg, const int& s_or_c_arg, double& t_max_out, double& t_out_out)
	{
	    bool if_t0 = true;
	    bool if_t1 = true;
	    double t0, t1;

	    s_or_c = s_or_c_arg;
	    x[0] = x_arg[0];
	    x[1] = x_arg[1];
	    x_max[0] = x_max_arg[0];
	    x_max[1] = x_max_arg[1];
	    x_min[0] = x_min_arg[0];
	    x_min[1] = x_min_arg[1];
	    direction[0] = direction_arg[0];
	    direction[1] = direction_arg[1];
	    end_point[0] = end_point_arg[0];
	    end_point[1] = end_point_arg[1];
	    if ((end_point[0]-x[0])*direction[0] + (end_point[1]-x[1])*direction[1] < 0) {
		direction[0] *= -1;
		direction[1] *= -1;
	    }
	    if (direction[0] == 0) {
		if_t0 = false;
	    } else {
		if (direction[0] > 0) {
		    t0 = (x_max[0] - x[0])/direction[0];
		} else {
		    t0 = (x_min[0] - x[0])/direction[0];
		}
	    }
	    if (direction[1] == 0) {
		if_t1 = false;
	    } else {
		if (direction[1] > 0) {
		    t1 = (x_max[1] - x[1])/direction[1];
		} else {
		    t1 = (x_min[1] - x[1])/direction[1];
		}
	    }
	    if (if_t0 && if_t1) {
		t_out = std::min(t0, t1);
	    } else {
		if (if_t0) {
		    t_out = t0;
		} else {
		    t_out = t1;
		}
	    }
	    x_out[0] = x[0] + t_out*direction[0];
	    x_out[1] = x[1] + t_out*direction[1];
	    t_max = t_out + 1;
	    t_max_out = t_max;
	    t_out_out = t_out;
	}


	// Compute x=(s,c) for a given t (t is the parameter for the piecewise linear curve)
	void compute_new_x(double* x_new, const double t) {
	    if (t <= t_out) {
		x_new[0] = x[0] + t*direction[0];
		x_new[1] = x[1] + t*direction[1];
	    } else {
		x_new[0] = 1/(t_max-t_out)*((t_max - t)*x_out[0] + end_point[0]*(t - t_out));
		x_new[1] = 1/(t_max-t_out)*((t_max - t)*x_out[1] + end_point[1]*(t - t_out));
	    }
	}



	double operator()(double t) const
	{
	    double s;
	    double c;
	    if (t <= t_out) {
		s = x[0] + t*direction[0];
		c = x[1] + t*direction[1];
	    } else {
		s = 1/(t_max-t_out)*((t_max - t)*x_out[0] + end_point[0]*(t - t_out));
		c = 1/(t_max-t_out)*((t_max - t)*x_out[1] + end_point[1]*(t - t_out));
	    }
	    if (s_or_c == 0) {
		return s - s0 +  dtpv*(outflux*tm.fracFlow(s, c, cell) + influx);
	    } else if (s_or_c == 1) {
		double ff = tm.fracFlow(s, c, cell);
		double mc = tm.computeMc(c);
		double dps = tm.polyprops_.dps;
		double rhor = tm.polyprops_.rhor;
		double ads0 = tm.polyprops_.adsorbtion(std::max(c0, cmax0));
		double ads = tm.polyprops_.adsorbtion(std::max(c, cmax0));
		return (s - dps)*c - (s0 - dps)*c0
		    + rhor*((1.0 - porosity)/porosity)*(ads - ads0)
		    + dtpv*(outflux*ff*mc + influx_polymer);
	    } else {
		std::cout << "problem!" << std::endl;
	    }
	}
    };

    void TransportModelPolymer::solveSingleCell(const int cell)
    {
	if (method_ == 1) {
	    solveSingleCellBracketing(cell);
	} else if (method_ == 2) {
	    solveSingleCellSplitting(cell);
	} else {
	    THROW("Method is " << method_ << "");
	}
    }

    void TransportModelPolymer::solveSingleCellBracketing(int cell)
    {
	    ResidualC res(*this, cell);
	    const double a = 0.0;
	    const double b = polyprops_.c_max_limit;
	    int iters_used;
	    concentration_[cell] = modifiedRegulaFalsi(res, a, b, maxit_, tol_, iters_used);
	    cmax_[cell] = std::max(cmax_[cell], concentration_[cell]);
	    saturation_[cell] = res.lastSaturation();
	    fractionalflow_[cell] = fracFlow(saturation_[cell], concentration_[cell], cell);
	    mc_[cell] = computeMc(concentration_[cell]);
    }


    // Splitting method, where we compute alternatively the zeros for the residual in s and c along
    // a specified piecewise linear curve. At each iteration, we use a robust 1d solver.

    void TransportModelPolymer::solveSingleCellSplitting(int cell)
    {
	const int max_iters_falsi = 20;
	const double tol = 1e-7;
	int iters_used_falsi = 0;
	const int max_iters_split = 20;
	int iters_used_split = 0;

	Residual residual(*this, cell);
	ResidualSDir residual_s_dir(*this, cell);
	ResidualCDir residual_c_dir(*this, cell);
	// const int sdir = 0;
	// const int cdir = 1;
	// ResidualDir residual_dir(*this, cell);
	double x[2] = {saturation_[cell], concentration_[cell]};
	double res[2];
	residual.computeResidual(x, res);

	if (norm(res) < tol) {
	    return;
	}

	bool res_s_done;
	double x_min[2] = {0.0, 0.0};
	double x_max[2] = {1.0, polyprops_.c_max_limit};
	double t;
	double t_max;
	double t_out;
	double direction[2];
	double end_point[2];
	double gradient[2];

	if (std::abs(res[0]) < std::abs(res[1])) {
	    if (std::abs(res[0]) > tol) {
		if (res[0] < 0) {
		    end_point[0] = x_max[0];
		    end_point[1] = x_min[1];
		    direction[0] = end_point[0] - x[0];
		    direction[1] = end_point[1] - x[1];
		    residual_s_dir.setup(x, direction, end_point, x_min, x_max, t_max, t_out);
		    if (residual_s_dir(t_out) >= 0) {
			t_max = t_out;
		    }
		} else {
		    end_point[0] = x_min[0];
		    end_point[1] = x_max[1];
		    direction[0] = end_point[0] - x[0];
		    direction[1] = end_point[1] - x[1];
		    residual_s_dir.setup(x, direction, end_point, x_min, x_max, t_max, t_out);
		    if (residual_s_dir(t_out) <= 0) {
			t_max = t_out;
		    }
		}
		t = modifiedRegulaFalsi(residual_s_dir, 0., t_max, max_iters_falsi, tol, iters_used_falsi);
		if (std::abs(residual_s_dir(t)) > tol) {
		    std::cout << "modifiedRegulaFalsi did not produce result under tolerance." << std::endl;
		}
		residual_s_dir.compute_new_x(x, t);
	    }
	    res_s_done = true;
	    residual.computeGradient(x, res, gradient, true, 1);
	} else {
	    if (std::abs(res[1]) > tol) {
		if (res[1] < 0) {
		    end_point[0] = x_max[0];
		    end_point[1] = x_max[1];
		    direction[0] = end_point[0] - x[0];
		    direction[1] = end_point[1] - x[1];
		    residual_c_dir.setup(x, direction, end_point, x_min, x_max, t_max, t_out);
		    if (residual_c_dir(t_out) >= 0) {
			t_max = t_out;
		    }
		} else {
		    end_point[0] = x_min[0];
		    end_point[1] = x_min[1];
		    direction[0] = end_point[0] - x[0];
		    direction[1] = end_point[1] - x[1];
		    residual_c_dir.setup(x, direction, end_point, x_min, x_max, t_max, t_out);
		    if (residual_c_dir(t_out) <= 0) {
			t_max = t_out;
		    }
		}
		t = modifiedRegulaFalsi(residual_c_dir, 0., t_max, max_iters_falsi, tol, iters_used_falsi);
		if (std::abs(residual_c_dir(t)) > tol) {
		    std::cout << "modifiedRegulaFalsi did not produce result under tolerance." << std::endl;
		}
		residual_c_dir.compute_new_x(x, t);
	    }
	    res_s_done = false;
	    residual.computeGradient(x, res, gradient, false, 1);
	}

	while ((norm(res) > tol) && (iters_used_split < max_iters_split)) {
	    if (res_s_done) { // solve for c-residual
		direction[0] = -gradient[1];
		direction[1] = gradient[0];
		if (res[1] < 0) {
		    // We update the bounding box (Here we assume that the curve res_s(s,c)=0 is
		    // increasing). We do it only for a significantly large res[1]
		    if (res[1] < -tol ) {
			x_min[0] = x[0];
			x_min[1] = x[1];
		    }
		    //
		    end_point[0] = x_max[0];
		    end_point[1] = x_max[1];
		    residual_c_dir.setup(x, direction, end_point, x_min, x_max, t_max, t_out);
		    if (residual_c_dir(t_out) >= 0) {
			t_max = t_out;
		    }
		} else {
		    // We update the bounding box (Here we assume that the curve res_s(s,c)=0 is increasing)
		    if (res[1] > tol) {
			x_max[0] = x[0];
			x_max[1] = x[1];
		    }
		    //
		    end_point[0] = x_min[0];
		    end_point[1] = x_min[1];
		    residual_c_dir.setup(x, direction, end_point, x_min, x_max, t_max, t_out);
		    if (residual_c_dir(t_out) <= 0) {
			t_max = t_out;
		    }
		}
		t = modifiedRegulaFalsi(residual_c_dir, 0., t_max, max_iters_falsi, tol, iters_used_falsi);
		if (std::abs(residual_c_dir(t)) > tol) {
		    std::cout << "modifiedRegulaFalsi did not produce result under tolerance." << std::endl;
		}
		residual_c_dir.compute_new_x(x, t);
		residual.computeGradient(x, res, gradient, false, 1);
		res_s_done = false;
	    } else { // solve for s residual
		direction[0] = gradient[1];
		direction[1] = -gradient[0];
		if (res[0] < 0) {
		    end_point[0] = x_max[0];
		    end_point[1] = x_min[1];
		    residual_s_dir.setup(x, direction, end_point, x_min, x_max, t_max, t_out);
		    if (residual_s_dir(t_out) >= 0) {
			t_max = t_out;
		    }
		} else {
		    end_point[0] = x_min[0];
		    end_point[1] = x_max[1];
		    residual_s_dir.setup(x, direction, end_point, x_min, x_max, t_max, t_out);
		    if (residual_s_dir(t_out) <= 0) {
			t_max = t_out;
		    }
		}
		t = modifiedRegulaFalsi(residual_s_dir, 0., t_max, max_iters_falsi, tol, iters_used_falsi);
		if (std::abs(residual_s_dir(t)) > tol) {
		    std::cout << "modifiedRegulaFalsi did not produce result under tolerance." << std::endl;
		}
		residual_s_dir.compute_new_x(x, t);
		res_s_done = true;
		residual.computeGradient(x, res, gradient, true, 1);
	    }

	    iters_used_split += 1;
	}

	if ((iters_used_split >=  max_iters_split) && (norm(res) >= tol)) {
	    solveSingleCellBracketing(cell);
	    std::cout << "splitting did not work" << std::endl;
	} else {
	    concentration_[cell] = x[1];
	    cmax_[cell] = std::max(cmax_[cell], concentration_[cell]);
	    saturation_[cell] = x[0];
	    fractionalflow_[cell] = fracFlow(saturation_[cell], concentration_[cell], cell);
	    mc_[cell] = computeMc(concentration_[cell]);
	}
    }

    void TransportModelPolymer::solveMultiCell(const int num_cells, const int* cells)
    {
	double max_s_change = 0.0;
	double max_c_change = 0.0;
	int num_iters = 0;
	// Must store state variables before we start.
	std::vector<double> s0(num_cells);
	std::vector<double> c0(num_cells);
	std::vector<double> cmax0(num_cells);
	// Must set initial fractional flows etc. before we start.
	for (int i = 0; i < num_cells; ++i) {
	    const int cell = cells[i];
	    fractionalflow_[cell] = fracFlow(saturation_[cell], concentration_[cell], cell);
	    mc_[cell] = computeMc(concentration_[cell]);
	    s0[i] = saturation_[cell];
	    c0[i] = concentration_[cell];
	    cmax0[i] = cmax_[i];
	}
	do {
	    int max_s_change_cell = -1;
	    int max_c_change_cell = -1;
	    max_s_change = 0.0;
	    max_c_change = 0.0;
	    for (int i = 0; i < num_cells; ++i) {
		const int cell = cells[i];
		const double old_s = saturation_[cell];
		const double old_c = concentration_[cell];
		saturation_[cell] = s0[i];
		concentration_[cell] = c0[i];
		cmax_[cell] = cmax0[i];
		solveSingleCell(cell);
		// std::cout << "cell = " << cell << "    delta s = " << saturation_[cell] - old_s << std::endl;
		if (max_s_change < std::fabs(saturation_[cell] - old_s)) {
		    max_s_change_cell = cell;
		}
		if (max_c_change < std::fabs(concentration_[cell] - old_c)) {
		    max_c_change_cell = cell;
		}
		max_s_change = std::max(max_s_change, std::fabs(saturation_[cell] - old_s));
		max_c_change = std::max(max_c_change, std::fabs(concentration_[cell] - old_c));
	    }
	    // std::cout << "Iter = " << num_iters << "    max_s_change = " << max_s_change
	    // 	      << "    in cell " << max_change_cell << std::endl;
	} while (((max_s_change > tol_) || (max_c_change > tol_)) && ++num_iters < maxit_);
	if (max_s_change > tol_) {
	    THROW("In solveMultiCell(), we did not converge after "
		  << num_iters << " iterations. Delta s = " << max_s_change);
	}
	if (max_c_change > tol_) {
	    THROW("In solveMultiCell(), we did not converge after "
		  << num_iters << " iterations. Delta c = " << max_c_change);
	}
	std::cout << "Solved " << num_cells << " cell multicell problem in "
		  << num_iters << " iterations." << std::endl;
    }




    double TransportModelPolymer::fracFlow(double s, double c, int cell) const
    {
	double c_max_limit = polyprops_.c_max_limit;
	double cbar = c/c_max_limit;
	double mu_w = visc_[0];
	double mu_m = polyprops_.viscMult(c)*mu_w;
	double mu_p = polyprops_.viscMult(polyprops_.c_max_limit)*mu_w;
	double omega = polyprops_.omega;
	double mu_m_omega = std::pow(mu_m, omega);
	double mu_w_e   = mu_m_omega*std::pow(mu_w, 1.0 - omega);
	double mu_p_eff = mu_m_omega*std::pow(mu_p, 1.0 - omega);
	double inv_mu_w_eff = (1.0 - cbar)/mu_w_e + cbar/mu_p_eff;
	double inv_visc_eff[2] = { inv_mu_w_eff, 1.0/visc_[1] };
	double sat[2] = { s, 1.0 - s };
	double mob[2];
	props_.relperm(1, sat, &cell, mob, 0);
	mob[0] *= inv_visc_eff[0];
	mob[1] *= inv_visc_eff[1];
	return mob[0]/(mob[0] + mob[1]);
    }

    double TransportModelPolymer::fracFlowWithDer(double s, double c, int cell, double* der) const
    {
	// We should check the dimension of der
	double c_max_limit = polyprops_.c_max_limit;
	double cbar = c/c_max_limit;
	double mu_w = visc_[0];
	double mu_m_dc; // derivative of mu_m with respect to c
	double mu_m = polyprops_.viscMultWithDer(c, &mu_m_dc)*mu_w;
	mu_m_dc *= mu_w;
	double mu_p = polyprops_.viscMult(polyprops_.c_max_limit)*mu_w;
	double omega = polyprops_.omega;
	double mu_m_omega = std::pow(mu_m, omega);
	double mu_m_omega_minus1 = std::pow(mu_m, omega - 1.0);
	double mu_w_omega = std::pow(mu_w, 1.0 - omega);
	double mu_w_e   = mu_m_omega*mu_w_omega;
	double mu_w_e_dc = omega*mu_m_dc*mu_m_omega_minus1*mu_w_omega;
	double mu_p_omega = std::pow(mu_p, 1.0 - omega);
	double mu_p_eff = mu_m_omega*mu_p_omega;
	double mu_p_eff_dc = omega*mu_m_dc*mu_m_omega_minus1*mu_p_omega;
	double mu_w_eff = 1./((1 - cbar)/mu_w_e + cbar/mu_p_eff);
	double inv_mu_w_eff_dc = -mu_w_e_dc/(mu_w_e*mu_w_e)*(1. - cbar) - mu_p_eff_dc/(mu_p_eff*mu_p_eff)*cbar + (1./mu_p_eff - 1./mu_w_e);
	double mu_w_eff_dc = -mu_w_eff*mu_w_eff*inv_mu_w_eff_dc;
	double visc_eff[2] = { mu_w_eff, visc_[1] };
	double sat[2] = { s, 1.0 - s };
	double mob[2];
	double mob_ds[2];
	double mob_dc[2];
	double perm[2];
	double perm_ds[4];
	props_.relperm(1, sat, &cell, perm, perm_ds);
	mob[0] = perm[0]/visc_eff[0];
	mob[1] = perm[1]/visc_eff[1];
	mob_ds[0] = perm_ds[0]/mu_w_eff;
	mob_ds[1] = perm_ds[1]/mu_w_eff;
	mob_dc[0] = - perm[0]*mu_w_eff_dc/(mu_w_eff*mu_w_eff);
	mob_dc[1] = - perm[1]*mu_p_eff_dc/(mu_p_eff*mu_p_eff);
	der[0] = (mob_ds[0]*mob[1] - mob_ds[1]*mob[0])/((mob[0] + mob[1])*(mob[0] + mob[1]));
	der[1] = (mob_dc[0]*mob[1] - mob_dc[1]*mob[0])/((mob[0] + mob[1])*(mob[0] + mob[1]));
 	return mob[0]/(mob[0] + mob[1]);
    }

    double TransportModelPolymer::computeMc(double c) const
    {
	double c_max_limit = polyprops_.c_max_limit;
	double cbar = c/c_max_limit;
	double mu_w = visc_[0];
	double mu_m = polyprops_.viscMult(c)*mu_w;
	double mu_p = polyprops_.viscMult(polyprops_.c_max_limit)*mu_w;
	double omega = polyprops_.omega;
	double mu_m_omega = std::pow(mu_m, omega);
	double mu_w_e   = mu_m_omega*std::pow(mu_w, 1.0 - omega);
	double mu_p_eff = mu_m_omega*std::pow(mu_p, 1.0 - omega);
	double inv_mu_w_eff = (1.0 - cbar)/mu_w_e + cbar/mu_p_eff;
	return c/(inv_mu_w_eff*mu_p_eff);
    }

    double TransportModelPolymer::computeMcWithDer(double c, double* der) const
    {
	double c_max_limit = polyprops_.c_max_limit;
	double cbar = c/c_max_limit;
	double mu_w = visc_[0];
	double mu_m_dc; // derivative of mu_m with respect to c
	double mu_m = polyprops_.viscMultWithDer(c, &mu_m_dc)*mu_w;
	mu_m_dc *= mu_w;
	double mu_p = polyprops_.viscMult(polyprops_.c_max_limit)*mu_w;
	double omega = polyprops_.omega;
	double mu_m_omega = std::pow(mu_m, omega);
	double mu_m_omega_minus1 = std::pow(mu_m, omega-1);
	double mu_w_omega = std::pow(mu_w, 1.0 - omega);
	double mu_w_e   = mu_m_omega*mu_w_omega;
	double mu_w_e_dc = omega*mu_m_dc*mu_m_omega_minus1*mu_w_omega;
	double mu_p_omega = std::pow(mu_p, 1.0 - omega);
	double mu_p_eff = mu_m_omega*mu_p_omega;
	double mu_p_eff_dc = omega*mu_m_dc*mu_m_omega_minus1*mu_p_omega;
	double mu_w_eff = 1./((1 - cbar)/mu_w_e + cbar/mu_p_eff);
	double inv_mu_w_eff_dc = -mu_w_e_dc/(mu_w_e*mu_w_e)*(1. - cbar) - mu_p_eff_dc/(mu_p_eff*mu_p_eff)*cbar + (1./mu_p_eff - 1./mu_w_e);
	double mu_w_eff_dc = -mu_w_eff*mu_w_eff*inv_mu_w_eff_dc;
	*der = mu_w_eff/mu_p_eff + c*mu_w_eff_dc/mu_p_eff - c*mu_p_eff_dc*mu_w_eff/(mu_p_eff*mu_p_eff);
	return c*mu_w_eff/mu_p_eff;
    }

} // namespace Opm


static double norm(double* res) {
    double absres0 = std::abs(res[0]);
    double absres1 = std::abs(res[1]);
    if (absres0 <= absres1) {
	return absres1;
    }
    else {
	return absres0;
    }
}




/* Local Variables:    */
/* c-basic-offset:4    */
/* End:                */