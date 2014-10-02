#include "mfg.h"
#include "mfg_utils.h"

#define LNERR_SAMPLE

struct Data_BA_PTLNVP
{
	int numView, frontPosIdx, frontFrmIdx;
	cv::Mat					K;
	vector<KeyPoint3d>&		kp;    // key points in map
	vector<int>				kptIdx2Opt;    // idx of key points to optimize
	vector<int>				kptIdx2Rpj_notOpt; 
	vector<cv::Mat>			prevPs;		// projection matrices of previous frames
	vector<View>&			views;
	int numVP;
	vector<IdealLine3d>&	il;
	vector<int>				idlIdx2Opt;
	vector<int>				idlIdx2Rpj_notOpt;

	// for debugging puropse
	double* ms; 
	double err_pt, err_ln, err_all;
	double err_pt_mean, err_ln_mean;

	Data_BA_PTLNVP(vector<KeyPoint3d>& kp_, vector<IdealLine3d>& il_, vector<View>& views_) :
			kp(kp_), il(il_), views(views_) {}

};

void costFun_BA_PtLnVp(double *p, double *error, int numPara, int numMeas, void *adata)
{
	struct Data_BA_PTLNVP * dp = (struct Data_BA_PTLNVP *) adata;
	double kernelPt = 2;
	double kernelLn = 5;
	double kernelVpLn = 5;

	// ----- recover parameters for each view and landmark -----
	// ---- pose para ----
	int pidx = 0;
	vector<cv::Mat>  Ps(dp->numView); // projection matrices
	for (int i = dp->frontFrmIdx, j=0; i < dp->frontPosIdx; ++i, ++j)	{
		Ps[i] = dp->prevPs[j];
	}
	Ps[0] = dp->K*(cv::Mat_<double>(3,4)<< 1, 0, 0, 0,
									       0, 1, 0, 0,
									       0, 0, 1, 0);
	for (int i = dp->frontPosIdx; i < dp->numView; ++i) {
		Quaterniond q( p[pidx], p[pidx+1], p[pidx+2], p[pidx+3]);
		q.normalize();
		cv::Mat Ri = (cv::Mat_<double>(3,3) 
			<< q.matrix()(0,0), q.matrix()(0,1), q.matrix()(0,2),
			q.matrix()(1,0), q.matrix()(1,1), q.matrix()(1,2),
			q.matrix()(2,0), q.matrix()(2,1), q.matrix()(2,2));
		cv::Mat ti;
		if (i==1) {
			ti = angle2unitVec (p[pidx+4],p[pidx+5]);		
			pidx = pidx + 6;
		} else {
			ti = (cv::Mat_<double>(3,1)<<p[pidx+4],p[pidx+5],p[pidx+6]);
			pidx = pidx + 7;
		}	
		cv::Mat P(3,4,CV_64F);
		Ri.copyTo(P.colRange(0,3));
		ti.copyTo(P.col(3));
		P = dp->K * P;
		Ps[i] = P.clone();
	}

	
	vector<double> err_pt_vec, err_ln_vec;

	// ----- reproject points -----		
	int eidx = 0;
	for(int i=0; i < dp->kptIdx2Opt.size(); ++i) {
		int idx =  dp->kptIdx2Opt[i];
		for(int j=0; j < dp->kp[idx].viewId_ptLid.size(); ++j) {
			int vid = dp->kp[idx].viewId_ptLid[j][0];
			int lid = dp->kp[idx].viewId_ptLid[j][1];
			if (vid >= dp->frontFrmIdx) {
				cv::Mat impt = Ps[vid] *(cv::Mat_<double>(4,1)<<p[pidx],p[pidx+1],p[pidx+2],1);
				error[eidx] = mat2cvpt(impt).x - dp->views[vid].featurePoints[lid].x;
				error[eidx+1] = mat2cvpt(impt).y - dp->views[vid].featurePoints[lid].y;
				error[eidx] = sqrt(pesudoHuber(error[eidx], kernelPt));
				error[eidx+1] = sqrt(pesudoHuber(error[eidx+1], kernelPt));

				err_pt_vec.push_back(error[eidx]*error[eidx] + error[eidx+1]*error[eidx+1]);
				
				eidx = eidx + 2;
			}
		}
		pidx = pidx + 3;
	}
	for(int i=0; i < dp->kptIdx2Rpj_notOpt.size(); ++i) {
		int idx =  dp->kptIdx2Rpj_notOpt[i];
		for(int j=0; j < dp->kp[idx].viewId_ptLid.size(); ++j) {
			int vid = dp->kp[idx].viewId_ptLid[j][0];
			int lid = dp->kp[idx].viewId_ptLid[j][1];
			if (vid >= dp->frontFrmIdx) {
				cv::Mat impt = Ps[vid] *  dp->kp[idx].mat(); // just provide reprojection
				error[eidx] = mat2cvpt(impt).x - dp->views[vid].featurePoints[lid].x;
				error[eidx+1] = mat2cvpt(impt).y - dp->views[vid].featurePoints[lid].y;
				error[eidx] = sqrt(pesudoHuber(error[eidx], kernelPt));
				error[eidx+1] = sqrt(pesudoHuber(error[eidx+1], kernelPt));

				err_pt_vec.push_back(error[eidx]*error[eidx] + error[eidx+1]*error[eidx+1]);
				
				eidx = eidx + 2;
			}
		}
	}
	
	vector<cv::Mat> VPs;
	for(int i=0; i < dp->numVP; ++i) {
		cv::Mat vp = (cv::Mat_<double>(4,1)<< p[pidx],p[pidx+1],p[pidx+2],0);
		VPs.push_back(vp);
		pidx = pidx + 3;
	}
	// ----- reproject lines -----
	for(int i=0; i < dp->idlIdx2Opt.size(); ++i) {
		int idx = dp->idlIdx2Opt[i];
		cv::Mat E1 = (cv::Mat_<double>(4,1)<< p[pidx], p[pidx+1], p[pidx+2], 1);   // line extremites
		cv::Mat E2 = VPs[dp->il[idx].vpGid];  // use vp direction as line's
		pidx = pidx + 3;
		for (int j=0; j < dp->il[idx].viewId_lnLid.size(); ++j) {
			int vid = dp->il[idx].viewId_lnLid[j][0];
			if (vid >= dp->frontFrmIdx) {
				cv::Mat pe1 = Ps[vid] * E1;
				cv::Mat pe2 = Ps[vid] * E2;
				cv::Mat lneq = (pe1).cross(pe2);
				int lid = dp->il[idx].viewId_lnLid[j][1];	
#ifdef LNERR_SAMPLE
				for(int k=0; k < dp->views[vid].idealLines[lid].lsEndpoints.size(); k=k+2) {
					cv::Point2d ep1 = dp->views[vid].idealLines[lid].lsEndpoints[k];
					cv::Point2d ep2 = dp->views[vid].idealLines[lid].lsEndpoints[k+1];
					error[eidx] = ls2lnArea(lneq, ep1, ep2)/cv::norm(ep1-ep2)/2;   // normalized by ls length
					error[eidx] = sqrt(pesudoHuber(error[eidx], kernelLn));
					error[eidx+1] = error[eidx];

					err_ln_vec.push_back(error[eidx]*error[eidx]);  
					err_ln_vec.push_back(error[eidx+1]*error[eidx+1]);// for each line segment

					eidx = eidx + 2;
				}
#else
				for(int k=0; k < dp->views[vid].idealLines[lid].lsEndpoints.size(); k=k+1) {
					// endpoint to line(projected) distance
					error[eidx] = point2LineDist(lneq, dp->views[vid].idealLines[lid].lsEndpoints[k]);
					error[eidx] = sqrt(pesudoHuber(error[eidx], kernelLn));
					
					err_ln_vec.push_back(error[eidx]*error[eidx]); 

					++eidx;
				}
#endif
			}
		}
	}
	for(int i=0; i < dp->idlIdx2Rpj_notOpt.size(); ++i) {
		int idx = dp->idlIdx2Rpj_notOpt[i];
		for (int j=0; j < dp->il[idx].viewId_lnLid.size(); ++j) {
			int vid = dp->il[idx].viewId_lnLid[j][0];
			if (vid >= dp->frontFrmIdx) {
				cv::Mat lneq = projectLine(dp->il[idx],Ps[vid]);
				int lid = dp->il[idx].viewId_lnLid[j][1];
#ifdef LNERR_SAMPLE
				for(int k=0; k < dp->views[vid].idealLines[lid].lsEndpoints.size(); k=k+2) {
					cv::Point2d ep1 = dp->views[vid].idealLines[lid].lsEndpoints[k];
					cv::Point2d ep2 = dp->views[vid].idealLines[lid].lsEndpoints[k+1];
					error[eidx] = ls2lnArea(lneq, ep1, ep2)/cv::norm(ep1-ep2)/2;   // normalized by ls length
					error[eidx] = sqrt(pesudoHuber(error[eidx], kernelLn));
					error[eidx+1] = error[eidx];

					err_ln_vec.push_back(error[eidx]*error[eidx]);  // for each linesegment
					err_ln_vec.push_back(error[eidx+1]*error[eidx+1]);

					eidx = eidx + 2;
				}
#else
				for(int k=0; k < dp->views[vid].idealLines[lid].lsEndpoints.size(); k=k+1) {
					// endpoint to line(projected) distance
					error[eidx] = point2LineDist(lneq, dp->views[vid].idealLines[lid].lsEndpoints[k]);
					error[eidx] = sqrt(pesudoHuber(error[eidx], kernelLn));
					
					err_ln_vec.push_back(error[eidx]*error[eidx]); 

					++eidx;
				}
#endif
			}
		}
	}


	// ----- compute error -----
	double cost_pt = 0, cost_ln = 0, cost;
	for(int i=0; i < err_pt_vec.size(); ++i) {
		cost_pt = cost_pt + err_pt_vec[i];
	}
	for(int i=0; i < err_ln_vec.size(); ++i) {
		cost_ln = cost_ln + err_ln_vec[i];
	}
	
	dp->err_pt = cost_pt;
	dp->err_ln = cost_ln;
	dp->err_pt_mean = cost_pt/err_pt_vec.size();
	dp->err_ln_mean = cost_ln/(err_ln_vec.size()/2); // average for each linesegment(consisting of 2 endpoints)

	cost = cost_pt + cost_ln;
	dp->err_all = cost;
	static int count = 0;
	count++;
	if(!(count%200))
		cout << cost << "\t";	
}

void Mfg::adjustBundle_PtLnVp ()
// local bundle adjustment: points
{
	// ----- BA setting -----
	int numPos = 3;		// number of camera poses to optimize 
	int numFrm = 5;	// number of frames to provide measurements (for reprojection error)
	// Note: numFrm should be larger or equal to numPos+2, to fix scale

	// ----- LM parameter setting -----
	double opts[LM_OPTS_SZ], info[LM_INFO_SZ];
	opts[0] = LM_INIT_MU; //
	opts[1] = 1E-10; // gradient threshold, original 1e-15
	opts[2] = 1E-50; // relative para change threshold? original 1e-50
	opts[3] = 1E-20; // error threshold (below it, stop)
	opts[4] = LM_DIFF_DELTA;
	int maxIter = 500;

	// ----- optimization parameters -----
	vector<double> paraVec; 
	// ---- camera pose parameters ----
	int frontPosIdx = max(1, (int)views.size() - numPos);
	int frontFrmIdx = max(0, (int)views.size() - numFrm);
	for(int i = frontPosIdx; i < views.size(); ++i) {	
		Quaterniond qi = r2q(views[i].R);
		paraVec.push_back(qi.w());
		paraVec.push_back(qi.x());
		paraVec.push_back(qi.y());
		paraVec.push_back(qi.z());
		if (i==1) {
			double alpha, beta;
			unitVec2angle(views[i].t, &alpha, &beta);
			paraVec.push_back(alpha);
			paraVec.push_back(beta);		
		} else {		
			paraVec.push_back(views[i].t.at<double>(0));
			paraVec.push_back(views[i].t.at<double>(1));
			paraVec.push_back(views[i].t.at<double>(2));
		}
	}
	// ---- structure parameters ----
	// --- points ---
	vector<int> kptIdx2Opt; // keyPoints idx to optimize 
	vector<int> kptIdx2Rpj_notOpt; // keypoint idx to reproject but not optimize
	for(int i=0; i < keyPoints.size(); ++i) {
		if(!keyPoints[i].is3D) continue;
		for(int j=0; j < keyPoints[i].viewId_ptLid.size(); ++j) {
			if (keyPoints[i].viewId_ptLid[j][0] >= frontPosIdx) { // point to optimize
		// don't optimize too-old (established before frontFrmIdx) points, 
		// but still use their recent observations/reprojections after frontPosIdx		
				if(1&& keyPoints[i].viewId_ptLid[0][0] < frontFrmIdx) {
					kptIdx2Rpj_notOpt.push_back(i);
				} else {
					paraVec.push_back(keyPoints[i].x);
					paraVec.push_back(keyPoints[i].y);
					paraVec.push_back(keyPoints[i].z);
					kptIdx2Opt.push_back(i);
				}
				break;
			}
		}		
	}
	// --- vanishing points ---
	for(int i=0; i < vanishingPoints.size(); ++i) {
		paraVec.push_back(vanishingPoints[i].x);
		paraVec.push_back(vanishingPoints[i].y);
		paraVec.push_back(vanishingPoints[i].z);
	}
	// --- lines ---
	vector<int> idlIdx2Opt;
	vector<int> idlIdx2Rpj_notOpt;
	for(int i=0; i < idealLines.size(); ++i) {
		if(!idealLines[i].is3D) continue;
		for(int j=0; j < idealLines[i].viewId_lnLid.size(); ++j) {
			if(idealLines[i].viewId_lnLid[j][0] >= frontPosIdx) {
				if(1&& idealLines[i].viewId_lnLid[0][0] < frontFrmIdx) {
					idlIdx2Rpj_notOpt.push_back(i);
				} else {
					paraVec.push_back(idealLines[i].midpt.x);
					paraVec.push_back(idealLines[i].midpt.y);
					paraVec.push_back(idealLines[i].midpt.z);
					idlIdx2Opt.push_back(i);
				}
				break;
			}
		}
	}

	int numPara = paraVec.size();
	double* para = new double[numPara];
	for (int i=0; i<numPara; ++i) {
		para[i] = paraVec[i];
	}

	// ----- optimization measurements -----
	vector<double> measVec;
	for(int i=0; i < kptIdx2Opt.size(); ++i) {
		for(int j=0; j < keyPoints[kptIdx2Opt[i]].viewId_ptLid.size(); ++j) {
			if(keyPoints[kptIdx2Opt[i]].viewId_ptLid[j][0] >= frontFrmIdx) {
				measVec.push_back(0);
				measVec.push_back(0);
			}
		}
	}
	for(int i=0; i < kptIdx2Rpj_notOpt.size(); ++i) {
		for(int j=0; j < keyPoints[kptIdx2Rpj_notOpt[i]].viewId_ptLid.size(); ++j) {
			if(keyPoints[kptIdx2Rpj_notOpt[i]].viewId_ptLid[j][0] >= frontFrmIdx) {
				measVec.push_back(0);
				measVec.push_back(0);
			}
		}
	}

	for(int i=0; i < idlIdx2Opt.size(); ++i) {
		for(int j=0; j < idealLines[idlIdx2Opt[i]].viewId_lnLid.size(); ++j) {
			if(idealLines[idlIdx2Opt[i]].viewId_lnLid[j][0] >= frontFrmIdx) {
				int vid = idealLines[idlIdx2Opt[i]].viewId_lnLid[j][0];
				int lid = idealLines[idlIdx2Opt[i]].viewId_lnLid[j][1];
				for(int k=0; k < views[vid].idealLines[lid].lsEndpoints.size(); ++k) {
					measVec.push_back(0);
				}
			}
		}
	}
	for(int i=0; i < idlIdx2Rpj_notOpt.size(); ++i) {
		for(int j=0; j < idealLines[idlIdx2Rpj_notOpt[i]].viewId_lnLid.size(); ++j) {
			if(idealLines[idlIdx2Rpj_notOpt[i]].viewId_lnLid[j][0] >= frontFrmIdx) {
				int vid = idealLines[idlIdx2Rpj_notOpt[i]].viewId_lnLid[j][0];
				int lid = idealLines[idlIdx2Rpj_notOpt[i]].viewId_lnLid[j][1];
				for(int k=0; k < views[vid].idealLines[lid].lsEndpoints.size(); ++k) {
					measVec.push_back(0);
				}
			}
		}
	}

	int numMeas = measVec.size();
	double* meas = new double[numMeas];
	for ( int i=0; i<numMeas; ++i) {
		meas[i] = measVec[i];
	}

	// ----- pass additional data -----
	Data_BA_PTLNVP data(keyPoints, idealLines, views);
	data.kptIdx2Opt = kptIdx2Opt;
	data.kptIdx2Rpj_notOpt = kptIdx2Rpj_notOpt;
	data.numView = views.size();
	data.frontPosIdx = frontPosIdx;
	data.frontFrmIdx = frontFrmIdx;
	data.K = K;
	data.ms = meas;
	data.numVP = vanishingPoints.size();

	data.idlIdx2Opt = idlIdx2Opt;
	data.idlIdx2Rpj_notOpt = idlIdx2Rpj_notOpt;
	for(int i=frontFrmIdx; i<frontPosIdx; ++i) {
		cv::Mat P(3,4,CV_64F);
		views[i].R.copyTo(P.colRange(0,3));
		views[i].t.copyTo(P.col(3));
		P = K * P;
		data.prevPs.push_back(P);
	}
	
	// ----- start LM solver -----
	MyTimer timer;
	timer.start();
	cout<<"View "+num2str(views.back().id)<<", paraDim="<<numPara<<", measDim="<<numMeas<<endl;
	int ret = dlevmar_dif(costFun_BA_PtLnVp, para, meas, numPara, numMeas,
						maxIter, opts, info, NULL, NULL, (void*)&data);
	timer.end();
	cout<<"\n Time used: "<<timer.time_s<<" sec. ";
	termReason((int)info[6]);
	delete[] meas;	

	// ----- update camera and structure parameters -----
	int pidx = 0;
	for(int i = frontPosIdx; i < views.size(); ++i) {	
		Quaterniond q( para[pidx], para[pidx+1], para[pidx+2], para[pidx+3]);
		q.normalize();
		views[i].R = (cv::Mat_<double>(3,3) 
			<< q.matrix()(0,0), q.matrix()(0,1), q.matrix()(0,2),
			q.matrix()(1,0), q.matrix()(1,1), q.matrix()(1,2),
			q.matrix()(2,0), q.matrix()(2,1), q.matrix()(2,2));
		if (i==1) {
			views[i].t = angle2unitVec(para[pidx+4],para[pidx+5]);
			pidx = pidx + 6;
		} else {
			views[i].t = (cv::Mat_<double>(3,1)<<para[pidx+4],para[pidx+5],para[pidx+6]);
			pidx = pidx + 7;
		}
	}
	// ---- structure parameters ----
	for(int i=0; i < kptIdx2Opt.size(); ++i) {
		int idx = kptIdx2Opt[i];
		keyPoints[idx].x = para[pidx];
		keyPoints[idx].y = para[pidx+1];
		keyPoints[idx].z = para[pidx+2];
		pidx = pidx + 3;
	}
	for(int i=0; i < vanishingPoints.size(); ++i) {
		vanishingPoints[i].x = para[pidx];
		vanishingPoints[i].y = para[pidx+1];
		vanishingPoints[i].z = para[pidx+2];
		pidx = pidx + 3;
	}

	for ( int i=0; i < idlIdx2Opt.size(); ++i) {
		int idx = idlIdx2Opt[i];
		cv::Point3d oldMidPt = idealLines[idx].midpt;
		idealLines[idx].midpt.x = para[pidx];
		idealLines[idx].midpt.y = para[pidx+1];
		idealLines[idx].midpt.z = para[pidx+2];

		//update line direction
		idealLines[idx].direct = vanishingPoints[idealLines[idx].vpGid].mat();
		pidx = pidx + 3;
		
		// keep old midpt
		idealLines[idx].midpt = projectPt3d2Ln3d (idealLines[idx], oldMidPt);
	}

	// ----- write to file -----
	views.back().errAll = data.err_all;
	views.back().errPt = data.err_pt;
	views.back().errLn = data.err_ln;
	views.back().errPtMean = data.err_pt_mean;
	views.back().errLnMean = data.err_ln_mean;
//	exportCamPose (*this, num2str(views.back().id)+".txt");
	exportCamPose (*this, "latest.txt");
}



