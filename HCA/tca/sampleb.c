/*
 * Sampling utility for b
 * Copyright (C) 2011-2013 Wray Buntine 
 * All rights reserved.
 *
 * This Source Code Form is subject to the terms of the Mozilla 
 * Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at
 *      http://mozilla.org/MPL/2.0/.
 *
 * Author: Wray Buntine (wray.buntine@monash.edu)
 *
 */

#include <stdio.h>  
#include <unistd.h>   
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <assert.h>

#include "yap.h"
#include "lgamma.h"
#include "myarms.h"
#include "sample.h"
#include "psample.h"    //   from libstb
#include "stats.h"

#define HUGE_INCREASE 1e20

/*
 *  globals from tca.h and stats.h
 */
extern rngp_t rngp;
extern int verbose;

// #define B_DEBUG

/*
 *
 */
static double bterms_theta(double b, void *mydata) {
  int i;
  double val = pctl_gammaprior(b);
  double lgb = lgamma(b);
  double lgba = 0;
  double lb = 0;
  if ( ddP.a_theta>0 )
    lgba = lgamma(b/ddP.a_theta);
  else
    lb = log(b);
  for (i=0; i<ddN.DT; i++) {
    if ( ddP.a_theta>0 )
      val += gammadiff(ddS.C_dT[i],b/ddP.a_theta,lgba);
    else
      val += ddS.C_dT[i] * lb;
    val -= gammadiff(ddS.N_dT[i], b, lgb);
  }
  myarms_evals++;
#ifdef B_DEBUG
  yap_message("Eval bterms_theta(%lf) = %lf", b, val);
  ddP.b_theta = b;
  cache_update("bt");
  yap_message(", lp=%lf\n", likelihood());
#endif
  return val;
}

static double bterms_mu1(double b, void *mydata) {
  int e;
  double lgb = lgamma(b);
  double val = pctl_gammaprior(b);
  assert(ddN.E>1);
  for (e=1; e<ddN.E; e++) {
    val += poch(b, ddP.a_mu, ddS.Cp_e[e]);
    if (e<ddN.E-1) {
      val -= gammadiff(ddS.C_e[e]+ddS.Cp_e[e+1], b, lgb);
    } else {
      val -= gammadiff(ddS.C_e[e], b, lgb);
    }
  }
  myarms_evals++;
#ifdef B_DEBUG
  yap_message("Eval bterms_mu1(%lf) = %lf", b, val);
  for (e=1; e<ddN.E; e++)
    ddP.b_mu[e] = b;
  cache_update("bm1");
  yap_message(", lp=%lf\n", likelihood());
#endif
  return val;
}

static double bterms_mu0(double b, void *mydata) {
  double val = pctl_gammaprior(b);
  val += poch(b, ddP.a_mu, ddS.Cp_e[0]);
  val -= gammadiff(ddS.C_e[0]+(ddN.E>1)?ddS.Cp_e[1]:0, b, 0);
  myarms_evals++;
#ifdef B_DEBUG
  yap_message("Eval bterms_mu0(%lf) = %lf", b, val);
  ddP.b_mu[0] = b;
  cache_update("bm0");
  yap_message(", lp=%lf\n", likelihood());
#endif
  return val;
}

static double bterms_phi0(double b, void *mydata) {
  int t;
  double val = 0;
  double lgb = lgamma(b);
  for (t=0; t<ddN.T; t++) {
    val += pctl_gammaprior(b);
    val += poch(b, ddP.a_phi, ddS.S_eVt[0][t]);
    val -= gammadiff(ddS.M_eVt[0][t] + (ddN.E>1)?ddS.S_eVt[1][t]:0, 
                     b, lgb);
  }
  myarms_evals++;
#ifdef B_DEBUG
  yap_message("Eval bterms_phi0(%d,%lf) = %lf", k, b, val);
  for (t=0; t<ddN.T; t++)
    ddP.b_phi[0][t] = b;
  cache_update("bp0");
  yap_message(", lp=%lf\n", likelihood());
#endif
  return val;
}

static double bterms_phi1(double b, void *mydata) {
  int e;
  int k = *(int*)mydata;
  double val = pctl_gammaprior(b);
  double lgb = lgamma(b);
  for (e=0; e<ddN.E; e++) {
    val += poch(b, ddP.a_phi, ddS.S_eVt[e][k]);
    if (e<ddN.E-1) {
      val -= gammadiff(ddS.M_eVt[e][k] + ddS.S_eVt[e+1][k], b, lgb);
    } else {
      val -= gammadiff(ddS.M_eVt[e][k], b, lgb);
    }
  }
  myarms_evals++;
#ifdef B_DEBUG
  yap_message("Eval bterms_phi1(%d,%lf) = %lf", k, b, val);
  ddP.b_phi[1][k] = b;
  cache_update("bp1");
  yap_message(", lp=%lf\n", likelihood());
#endif
  return val;
}

static double bterms_burst(double b, void *mydata) {
  int i;
  double val = pctl_gammaprior(b);
  double lgb = lgamma(b);
  double lgba = 0;
  double lb = 0;
  if ( ddP.a_burst>0 )
    lgba = lgamma(b/ddP.a_burst);
  else
    lb = log(b);
  for (i=0; i<ddN.DT; i++) {
    if ( ddP.a_burst>0 )
      val += gammadiff(ddS.N_dT[i],b/ddP.a_burst, lgba);
    else
      val += ddS.N_dT[i] * lb;
    val -= gammadiff(ddD.N_dT[i], b, lgb);
  }
  myarms_evals++;
#ifdef B_DEBUG
  yap_message("Eval bterms_burst(%lf) = %lf", b, val);
  ddP.b_burst = b;
  cache_update("bb");
  yap_message(", lp=%lf\n", likelihood());
#endif
  return val;
}

void sample_bt(double *b) {
  double startlike;
  if ( verbose>1 ) {
    startlike = likelihood();
    yap_message("sample_bt (pre): b_theta=%lf, lp=%lf\n",
		*b, startlike);
  }
  myarmsMH(PYP_CONC_MIN, PYP_CONC_MAX, &bterms_theta, NULL, b, "bt", 1);
  ddP.b_theta = *b;
  cache_update("bt");
  if ( verbose>1 ) {
    double endlike = likelihood();
    yap_message("sample_bt (post): b_theta=%lf, lp=%lf\n",
		*b, endlike);
    if ( endlike < startlike-50 ) {
      yap_quit("Sampler failed due to huge decrease!\n");
    }
  }
}

void sample_bb(double *b) {
  double startlike;
  if ( verbose>1 ) {
    startlike = likelihood();
    yap_message("sample_bb (pre): b_burst=%lf, lp=%lf\n",
		*b, startlike);
  }
  myarmsMH(PYP_CONC_MIN, PYP_CONC_MAX, &bterms_burst, NULL, b, "bb", 1);
  ddP.b_burst = *b;
  cache_update("bb");
  if ( verbose>1 ) {
    double endlike = likelihood();
    yap_message("sample_bb (post): b_burst=%lf, lp=%lf\n",
		*b, endlike);
    if ( endlike < startlike-50 ) {
      yap_quit("Sampler failed due to huge decrease!\n");
    }
  }
}

void sample_bm1(double *b) {
  double startlike;
  int e;
  if ( verbose>1 ) {
    startlike = likelihood();
    yap_message("sample_bm1 (pre): b_mu[1...]=%lf, lp=%lf\n",
		*b, startlike);
  }
  myarmsMH(PYP_CONC_MIN, PYP_CONC_MAX, &bterms_mu1, NULL, b, "bm1", 1);
  for (e=1; e<ddN.E; e++)
    ddP.b_mu[e] = *b;
  cache_update("bm1");
  if ( verbose>1 ) {
    double endlike = likelihood();
    yap_message("sample_bm1 (post): b_mu[1...]=%lf, lp=%lf\n",
		*b, endlike);
    if ( endlike < startlike-50 ) {
      yap_quit("Sampler failed due to huge decrease!\n");
    }
  }
}

void sample_bp1(double *b) {
  double startlike;
  int k;
  double bb;
  assert(ddN.E>1);
  for (k=0; k<ddN.T; k++) {
    bb = ddP.b_phi[1][k];
    if ( verbose>1 ) {
      startlike = likelihood();
      yap_message("sample_bp1 (pre): b_phi[1][%d]=%lf, lp=%lf\n",
		  k, bb, startlike);
    }
    myarmsMH(PYP_CONC_MIN, PYP_CONC_MAX, &bterms_phi1, &k, &bb, "bp1", 1);
    ddP.b_phi[1][k] = bb;
    cache_update("bp1");
    if ( verbose>1 ) {
      double endlike = likelihood();
      yap_message("sample_bp1 (post): b_phi[1][%d]=%lf, lp=%lf\n",
		  k, bb, endlike);
      if ( endlike < startlike-HUGE_INCREASE ) {
	yap_quit("Sampler failed due to huge decrease!\n");
      }
    }
  }
}

void sample_bp0(double *b) {
  double startlike;
  int t;
  if ( verbose>1 ) {
    startlike = likelihood();
    yap_message("sample_bp0 (pre): b_phi[0][0]=%lf, lp=%lf\n",
		*b, startlike);
  }
  myarmsMH(PYP_CONC_MIN, PYP_CONC_MAX, &bterms_phi0, NULL, b, "bp0", 1);
  for (t=0; t<ddN.T; t++)
    ddP.b_phi[0][t] = *b;
  cache_update("bp0");
  if ( verbose>1 ) {
    double endlike = likelihood();
    yap_message("sample_bp0 (post): b_phi[0][0]=%lf, lp=%lf\n",
		*b, endlike);
    if ( endlike < startlike-HUGE_INCREASE ) {
      yap_quit("Sampler failed due to huge decrease!\n");
    }
  }
}
void sample_bm0(double *b) {
  double startlike;
  if ( verbose>1 ) {
    startlike = likelihood();
    yap_message("sample_bm0 (pre): b_mu[0]=%lf, lp=%lf\n",
		*b, startlike);
  }
  myarmsMH(PYP_CONC_MIN, PYP_CONC_MAX, &bterms_mu0, NULL, b, "bm0", 1);
  ddP.b_mu[0] = *b;
  cache_update("bm0");
  if ( verbose>1 ) {
    double endlike = likelihood();
    yap_message("sample_bm0 (post): b_mu[0]=%lf, lp=%lf\n",
		*b, endlike);
    if ( endlike < startlike-HUGE_INCREASE ) {
      yap_quit("Sampler failed due to huge decrease!\n");
    }
  }
}


