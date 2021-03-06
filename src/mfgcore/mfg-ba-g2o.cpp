/////////////////////////////////////////////////////////////////////////////////
//
//  Multilayer Feature Graph (MFG), version 1.0
//  Copyright (C) 2011-2015 Yan Lu, Dezhen Song
//  Netbot Laboratory, Texas A&M University, USA
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//
/////////////////////////////////////////////////////////////////////////////////

/************************************************************************************************************************
 MFG bundle adjustment using G2O as solver and this file contains two functions in mfg.h:
 (1)bundle_adjust_between(int view_from, int view_to, int cam_from)
    Bundle adjust key-frames from "view_from" to "view_to".
    Camera pose parameters is set to be   fixed from "view_from" to "cam_from".
    Camera pose parameters is set to be unfixed from  "cam_from" to  "view_to".
 (2)adjustBundle_G2O(int numPos, int numFrm)
    Bundle adjust key-frames from the latest key-frames to previous n key-frames, n is a global variable. (ex:n=10)
    Paper refers to this function.

 Usually g2o is set up as follows:
 (1) Initialization   (optimizer, solver...)
 (2) Add g2o vertices (vertex setID --> vertex setestimate --> addVerte to optimizer)
 (3) Add g2o edges    (set vertices()[0] && vertices()[1] for edge-->edge setMeasurement-->addEdge to optimizer)
 (4) Run g2o optimization
 (5) Update vertices and edges
 ************************************************************************************************************************/

#include "mfg.h"
#include "utils.h"
#include "settings.h"

#include <Eigen/StdVector>
#include <stdint.h>

#ifdef _MSC_VER
//#include <unordered_set>
#include <unordered_map>
#else
// TODO: FIXME
//#include <tr1/unordered_set>
#include <unordered_map>
#endif

#include "g2o/config.h"
#include "g2o/core/block_solver.h"
#include "g2o/core/optimization_algorithm_levenberg.h"
#include "g2o/core/optimization_algorithm_gauss_newton.h"
#include "g2o/core/robust_kernel_impl.h"
#include "g2o/core/sparse_optimizer.h"
#include "g2o/core/solver.h"
#include "g2o/solvers/dense/linear_solver_dense.h"
#include "g2o/solvers/structure_only/structure_only_solver.h"
#include "g2o/types/icp/types_icp.h"
#include "g2o/types/sba/sbacam.h"

#if defined G2O_HAVE_CHOLMOD
#include "g2o/solvers/cholmod/linear_solver_cholmod.h"
#endif
#if defined G2O_HAVE_CSPARSE
#include "g2o/solvers/csparse/linear_solver_csparse.h"
#endif

#include "vertex_vnpt.h"
#include "vertex_plane.h"
#include "edge_vnpt_cam.h"
#include "edge_line_vp_cam.h"
#include "edge_point_plane.h"
#include "edge_line_vp_plane.h"
#include "edge_cam_cam_dist.h"
#include <fstream>
#include "levmar-2.6/levmar.h"

using namespace Eigen;
extern MfgSettings *mfgSettings;
extern bool mfg_writing;

void Mfg::bundle_adjust_between(int view_from, int view_to, int cam_from)
{
    // ----------------- G2O parameter setting -----------------//
    int maxIters = 25;
    // some handy typedefs
    typedef g2o::BlockSolver< g2o::BlockSolverTraits<Eigen::Dynamic, Eigen::Dynamic> >  MyBlockSolver;
    typedef g2o::LinearSolverCSparse<MyBlockSolver::PoseMatrixType> MyLinearSolver;
    typedef g2o::LinearSolverDense<MyBlockSolver::PoseMatrixType> MyDenseLinearSolver;

    // setup the solver
    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(false);
    MyLinearSolver *linearSolver = new MyLinearSolver();
    MyBlockSolver *solver_ptr = new MyBlockSolver(linearSolver);
    g2o::OptimizationAlgorithmLevenberg *solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    // add the parameter representing the sensor offset  !!!!
    g2o::ParameterSE3Offset *sensorOffset = new g2o::ParameterSE3Offset;
    sensorOffset->setOffset(Eigen::Isometry3d::Identity());
    sensorOffset->setId(0);
    optimizer.addParameter(sensorOffset);

    // set g2o vertices 
    int vertex_id = 0;
    int frontPosIdx = cam_from;
    int frontFrmIdx = view_from;
    int frontVptIdx = view_from; // first frame used for keep VP estimate consistent

    // -----------------   optimization parameters (1)~(2)  -----------------//
    // (1) add g2o vertices(camera pose parameters) to optimizer  
    vector<g2o::VertexCam *> camvertVec;           /// record every VertexCam to a vector
    unordered_map<int, int> camvid2fid, camfid2vid;/// record mapping between vertex_id and frame_id(view_from ~ view_to)

    for (int i = frontVptIdx; i <= view_to; ++i)
    {
        Eigen:: Quaterniond q = r2q(views[i].R);
        Eigen::Isometry3d pose;
        pose = q;
        pose.translation() = Eigen::Vector3d(views[i].t.at<double>(0), views[i].t.at<double>(1), views[i].t.at<double>(2));
        g2o::VertexCam *v_cam = new g2o::VertexCam();
        v_cam->setId(vertex_id);
        g2o::SBACam sc(q.inverse(), pose.inverse().translation());
        //	((g2o::SBACam*)v_cam)->setKcam(K.at<double>(0,0),K.at<double>(1,1),K.at<double>(0,2),K.at<double>(1,2),0);
        sc.setKcam(K.at<double>(0, 0), K.at<double>(1, 1), K.at<double>(0, 2), K.at<double>(1, 2), 0); 
        v_cam->setEstimate(sc);

        if (i < 1 || i < frontPosIdx)
            v_cam->setFixed(true);

        optimizer.addVertex(v_cam);
        camvid2fid[vertex_id] = i;
        camfid2vid[i] = vertex_id;
        ++vertex_id;
        camvertVec.push_back(v_cam);
    }

    // (2) add g2o edges(camera to camera distance constraints) to optimizer 
    vector<g2o::EdgeCamCamDist *> edges_camdist; /// record every EdgeCamCamDist to a vector

    for (int i = camdist_constraints.size() - 1; i >= 0 && i >= (int)camdist_constraints.size() - (view_to - view_from); --i)
    {
        if (camfid2vid.find((int)camdist_constraints[i][0]) != camfid2vid.end() &&
                camfid2vid.find((int)camdist_constraints[i][1]) != camfid2vid.end()
           )   // apply (hard) contraints
        {
            maxIters = max(20, view_to - view_from);
            //  optimizer.setVerbose(true);
            g2o::EdgeCamCamDist *e = new g2o::EdgeCamCamDist();
            e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(camfid2vid[(int)camdist_constraints[i][0]])->second);

            if (e->vertices()[0] == 0)
            {
                cerr << "no cam vertex found ... terminated \n";
                exit(0);
            }

            e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(camfid2vid[(int)camdist_constraints[i][1]])->second);

            if (e->vertices()[1] == 0)
            {
                cerr << "no cam vertex found ... terminated \n";
                exit(0);
            }

            e->setMeasurement(camdist_constraints[i][2]);
            e->information().setIdentity();
            e->information() = e->information() * camdist_constraints[i][3]; // observaion info
            optimizer.addEdge(e);
            edges_camdist.push_back(e);
        }
    }

    if (edges_camdist.size() > 0)
    {
        for (int i = 0; i < camvertVec.size(); ++i)
        {
            if (i > 0)
                camvertVec[i]->setFixed(false); //scale already fixed by dist-constaint, only need to fix one front camera
        }
    }

	
    // -----------------   structure parameters (1)~(2)  -----------------//
	// (1) add g2o vertices (keypoints, vanishpoints, ideal lines, primary planes) to optimizer  
    // ---- keypoints ----
    unordered_map<int, int> ptvid2gid, ptgid2vid;
    vector<g2o::VertexSBAPointXYZ *> ptvertVec;
    vector<int> kptIdx2Opt;        // keyPoint idx to optimize
    vector<int> kptIdx2Rpj_notOpt; // keypoint idx to reproject but not optimize

    // points-to-optimize contains those first appearing after frontFrmIdx and still being observed after frontPosIdx
    for (int i = 0; i < keyPoints.size(); ++i)
    {
        if (!keyPoints[i].is3D || keyPoints[i].gid < 0) continue;

        for (int j = 0; j < keyPoints[i].viewId_ptLid.size(); ++j)
        {
            if (keyPoints[i].viewId_ptLid[j][0] >= frontPosIdx)
            {
                // don't optimize too-old (established before frontFrmIdx) points,
                // but still use their recent observations/reprojections after frontPosIdx
                g2o::VertexSBAPointXYZ *v_p = new g2o::VertexSBAPointXYZ();
                v_p->setId(vertex_id);
                v_p->setMarginalized(true);
                Eigen::Vector3d pt(keyPoints[i].x, keyPoints[i].y, keyPoints[i].z);
                v_p->setEstimate(pt);

                if (keyPoints[i].viewId_ptLid[0][0] < frontFrmIdx
                        && keyPoints[i].estViewId < view_to
                        && edges_camdist.size() == 0 //
                   )
                {
                    kptIdx2Rpj_notOpt.push_back(i);
                    v_p->setFixed(true);
                }
                else     // not fixed, to be optimized
                {
                    v_p->setFixed(false);
                    kptIdx2Opt.push_back(i);
                    ptvertVec.push_back(v_p);
                }

                optimizer.addVertex(v_p);
                ptgid2vid[keyPoints[i].gid] = vertex_id;
                ptvid2gid[vertex_id] = keyPoints[i].gid;
                ++vertex_id;
                break;
            }
        }
    }

    // ---- vanishing points ----
    unordered_map<int, int> vpgid2vid, vpvid2gid;
    vector<int> vpIdx2Opt;
    vector<g2o::VertexVanishPoint *> vpvertVec;

    // vp that is observed in current window
    for (int i = 0; i < vanishingPoints.size(); ++i)
    {
        for (int j = 0; j < vanishingPoints[i].viewId_vpLid.size(); ++j)
        {
            if (vanishingPoints[i].viewId_vpLid[j][0] >= frontPosIdx)  // observed recently
            {
                vpIdx2Opt.push_back(i);
                g2o::VertexVanishPoint *v_vp = new g2o::VertexVanishPoint();
                v_vp->setId(vertex_id);
                Eigen::Vector3d pt(vanishingPoints[i].x, vanishingPoints[i].y, vanishingPoints[i].z);
                v_vp->setEstimate(pt);
                optimizer.addVertex(v_vp);
                vpvertVec.push_back(v_vp);
                vpvid2gid[vertex_id] = vanishingPoints[i].gid;
                vpgid2vid[vanishingPoints[i].gid] = vertex_id;
                ++vertex_id;
                break;
            }
        }
    }

    // ---- ideal lines ----
    unordered_map<int, int> lngid2vid, lnvid2gid;
    vector<int> lnIdx2Opt, lnIdx2Rpj_notOpt;// lnIdx2Opt idx to optimize && lnIdx2Rpj_notOpt idx to reproject but not optimize
    vector<g2o::VertexSBAPointXYZ *> lnvertVec;

    for (int i = 0; i < idealLines.size(); ++i)
    {
        if (!idealLines[i].is3D || idealLines[i].gid < 0) continue;

        for (int j = 0; j < idealLines[i].viewId_lnLid.size(); ++j)
        {
            if (idealLines[i].viewId_lnLid[j][0] >= frontPosIdx)
            {
                g2o::VertexSBAPointXYZ *v_lnpt = new g2o::VertexSBAPointXYZ();
                Vector3d pt(idealLines[i].midpt.x, idealLines[i].midpt.y, idealLines[i].midpt.z);
                v_lnpt->setEstimate(pt);
                v_lnpt->setId(vertex_id);
                lngid2vid[idealLines[i].gid] = vertex_id;
                lnvid2gid[vertex_id] = idealLines[i].gid;

                if (idealLines[i].viewId_lnLid[0][0] < frontFrmIdx
                        && idealLines[i].estViewId < view_to
                        && edges_camdist.size() == 0
                   )
                {
                    lnIdx2Rpj_notOpt.push_back(i);
                    v_lnpt->setFixed(true);
                }
                else
                {
                    lnIdx2Opt.push_back(i);
                    v_lnpt->setFixed(false);
                    lnvertVec.push_back(v_lnpt);
                }

                optimizer.addVertex(v_lnpt);
                ++vertex_id;
                break;
            }
        }
    }

    // ---- primary planes ----
    unordered_map<int, int> plgid2vid, plvid2gid;
    vector<int> plIdx2Opt;
    vector<g2o::VertexPlane3d *> plvertVec;

    for (int i = 0; i < primaryPlanes.size(); ++i)
    {
        if (view_to - primaryPlanes[i].estViewId < 3) continue;

        bool usePlane = false;

        for (int j = 0; j < primaryPlanes[i].kptGids.size(); ++j)
        {
            if (ptgid2vid.find(primaryPlanes[i].kptGids[j]) != ptgid2vid.end())
            {
                usePlane = true;
                break;
            }
        }

        for (int j = 0; j < primaryPlanes[i].ilnGids.size(); ++j)
        {
            if (lngid2vid.find(primaryPlanes[i].ilnGids[j]) != lngid2vid.end())
            {
                usePlane = true;
                break;
            }
        }

        if (!usePlane) continue;

        g2o::VertexPlane3d *v_pl = new g2o::VertexPlane3d();
        v_pl->setId(vertex_id);
        Eigen::Vector3d pl(primaryPlanes[i].n.at<double>(0) / primaryPlanes[i].d,
                           primaryPlanes[i].n.at<double>(1) / primaryPlanes[i].d,
                           primaryPlanes[i].n.at<double>(2) / primaryPlanes[i].d);
        v_pl->setEstimate(pl);

        if (primaryPlanes[i].estViewId < frontPosIdx)
            v_pl->setFixed(true); // don't change old planes
        else
            v_pl->setFixed(false);

        optimizer.addVertex(v_pl);
        plgid2vid[i] = vertex_id;
        plvid2gid[vertex_id] = i;
        plIdx2Opt.push_back(i);
        plvertVec.push_back(v_pl);
        ++vertex_id;
    }

	// (2) add g2o edges (keypoints, vanishpoints, lines, primary planes) to optimizer  
    vector<g2o::EdgeVnptCam *>      vecEdgeVnpt;
    vector<g2o::EdgeProjectP2MC *>  vecEdgeKpt;
    vector<g2o::EdgeLineVpCam *>    vecEdgeLine;
    vector<g2o::EdgePointPlane3d *> vecEdgePointPlane;
    vector<g2o::EdgeLineVpPlane *>  vecEdgeLinePlane;

    // ---- keypoints ----
    for (int i = 0; i < kptIdx2Opt.size(); ++i) // keyPoint idx to optimize
    {
        for (int j = 0; j < keyPoints[kptIdx2Opt[i]].viewId_ptLid.size(); ++j)
        {
            int fid = keyPoints[kptIdx2Opt[i]].viewId_ptLid[j][0]; //fram(view) id
            int lid = keyPoints[kptIdx2Opt[i]].viewId_ptLid[j][1];//local id

            if (!views[fid].matchable) continue;

            if (fid >= frontFrmIdx)
            {
                g2o::EdgeProjectP2MC *e = new g2o::EdgeProjectP2MC();
                e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(ptgid2vid[kptIdx2Opt[i]])->second);

                if (e->vertices()[0] == 0)
                {
                    cerr << "no pt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(camfid2vid[fid])->second);

                if (e->vertices()[1] == 0)
                {
                    cerr << "no cam vert found ... terminated \n";
                    exit(0);
                }

                Eigen::Vector2d meas(views[fid].featurePoints[lid].x, views[fid].featurePoints[lid].y);
                e->setMeasurement(meas);

                if (mfgSettings->getBaUseKernel())
                {
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    rk->setDelta(mfgSettings->getBaKernelDeltaPoint());
                    e->setRobustKernel(rk);
                }

                e->information() = Matrix2d::Identity();
                optimizer.addEdge(e);
                vecEdgeKpt.push_back(e);
            }
        }
    }

    for (int i = 0; i < kptIdx2Rpj_notOpt.size(); ++i) // keypoint idx to reproject but not optimize
    {
        for (int j = 0; j < keyPoints[kptIdx2Rpj_notOpt[i]].viewId_ptLid.size(); ++j)
        {
            int fid = keyPoints[kptIdx2Rpj_notOpt[i]].viewId_ptLid[j][0] ;
            int lid = keyPoints[kptIdx2Rpj_notOpt[i]].viewId_ptLid[j][1] ;

            if (!views[fid].matchable) continue;

            if (fid >= frontFrmIdx)
            {
                g2o::EdgeProjectP2MC *e = new g2o::EdgeProjectP2MC();
                e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(ptgid2vid[kptIdx2Rpj_notOpt[i]])->second);

                if (e->vertices()[0] == 0)
                {
                    cerr << "no pt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(camfid2vid[fid])->second);

                if (e->vertices()[1] == 0)
                {
                    cerr << "no cam vert found ... terminated \n";
                    exit(0);
                }

                Eigen::Vector2d meas(views[fid].featurePoints[lid].x, views[fid].featurePoints[lid].y);
                e->setMeasurement(meas);
                e->information() = Matrix2d::Identity();

                if (mfgSettings->getBaUseKernel())
                {
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    rk->setDelta(mfgSettings->getBaKernelDeltaPoint());
                    e->setRobustKernel(rk);
                }

                optimizer.addEdge(e);
                vecEdgeKpt.push_back(e);
            }
        }
    }

    // ---- vanishpoints ----
    for (int i = 0; i < vpIdx2Opt.size(); ++i)
    {
        int vpGid = vpIdx2Opt[i];

        for (int j = 0; j < vanishingPoints[vpGid].viewId_vpLid.size(); ++j)
        {
            int fid = vanishingPoints[vpGid].viewId_vpLid[j][0];
            int lid = vanishingPoints[vpGid].viewId_vpLid[j][1];

            if (!views[fid].matchable) continue;

            if (fid >= frontVptIdx)  // different from frontFrmIdx
            {
                g2o::EdgeVnptCam *e = new g2o::EdgeVnptCam();
                e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(vpgid2vid[vpGid])->second);

                if (e->vertices()[0] == 0)
                {
                    cerr << "no vpt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(camfid2vid[fid])->second);

                if (e->vertices()[1] == 0)
                {
                    cerr << "no cam vert found ... terminated \n";
                    exit(0);
                }

                cv::Mat univec_meas = K.inv() * views[fid].vanishPoints[lid].mat();
                univec_meas = univec_meas / cv::norm(univec_meas);
                double alpha, beta;
                unitVec2angle(univec_meas, &alpha, &beta);
                Eigen::Vector2d meas(alpha, beta);
                e->setMeasurement(meas);
                e->information() = Matrix2d::Identity();
                e->computeError();
                double err_raw = e->chi2();
                cv::Mat egvals;
                cv::eigen(views[fid].vanishPoints[lid].cov_pt(K), egvals);
                Matrix2d cov;
                cov(0, 0) = max(views[fid].vanishPoints[lid].cov_ab.at<double>(0, 0), 1e-3);
                cov(0, 1) = views[fid].vanishPoints[lid].cov_ab.at<double>(0, 1) * 0;
                cov(1, 0) = views[fid].vanishPoints[lid].cov_ab.at<double>(1, 0) * 0;
                cov(1, 1) = max(views[fid].vanishPoints[lid].cov_ab.at<double>(1, 1), 1e-3);
                e->information() = mfgSettings->getBaWeightVPoint() * cov.inverse(); // observaion info

                if (mfgSettings->getBaUseKernel())
                {
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    rk->setDelta(mfgSettings->getBaKernelDeltaVPoint());
                    e->setRobustKernel(rk);
                }

                optimizer.addEdge(e);
                vecEdgeVnpt.push_back(e);
            }
        }
    }

    // ---- lines ----
    for (int i = 0; i < lnIdx2Opt.size(); ++i)// lnIdx2Opt idx to optimize 
    {
        for (int j = 0; j < idealLines[lnIdx2Opt[i]].viewId_lnLid.size(); ++j)
        {
            int fid = idealLines[lnIdx2Opt[i]].viewId_lnLid[j][0];
            int lid = idealLines[lnIdx2Opt[i]].viewId_lnLid[j][1];

            if (!views[fid].matchable) continue;

            if (fid >= frontFrmIdx)
            {
                g2o::EdgeLineVpCam *e = new g2o::EdgeLineVpCam();
                e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(lngid2vid[lnIdx2Opt[i]])->second);

                if (e->vertices()[0] == 0)
                {
                    cerr << "no lnpt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(vpgid2vid[idealLines[lnIdx2Opt[i]].vpGid])->second);

                if (e->vertices()[1] == 0)
                {
                    cerr << "no vpt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[2] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(camfid2vid[fid])->second);

                if (e->vertices()[2] == 0)
                {
                    cerr << "no cam vert found ... terminated \n";
                    exit(0);
                }

                double meas = 0;
                e->setMeasurement(meas);
                e->information() = e->information() * mfgSettings->getBaWeightLine();

                if (mfgSettings->getBaUseKernel())
                {
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    rk->setDelta(mfgSettings->getBaKernelDeltaLine());
                    e->setRobustKernel(rk);
                }

                e->segpts = views[fid].idealLines[lid].lsEndpoints;
                optimizer.addEdge(e);
                vecEdgeLine.push_back(e);
            }
        }
    }

    for (int i = 0; i < lnIdx2Rpj_notOpt.size(); ++i)// lnIdx2Rpj_notOpt idx to reproject but not optimize
    {
        for (int j = 0; j < idealLines[lnIdx2Rpj_notOpt[i]].viewId_lnLid.size(); ++j)
        {
            int fid = idealLines[lnIdx2Rpj_notOpt[i]].viewId_lnLid[j][0];
            int lid = idealLines[lnIdx2Rpj_notOpt[i]].viewId_lnLid[j][1];

            if (!views[fid].matchable) continue;

            if (fid >= frontFrmIdx)
            {
                g2o::EdgeLineVpCam *e = new g2o::EdgeLineVpCam();
                e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(lngid2vid[lnIdx2Rpj_notOpt[i]])->second);

                if (e->vertices()[0] == 0)
                {
                    cerr << "no lnpt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(vpgid2vid[idealLines[lnIdx2Rpj_notOpt[i]].vpGid])->second);

                if (e->vertices()[1] == 0)
                {
                    cerr << "no vpt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[2] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(camfid2vid[fid])->second);

                if (e->vertices()[2] == 0)
                {
                    cerr << "no cam vert found ... terminated \n";
                    exit(0);
                }

                double meas = 0;
                e->setMeasurement(meas);
                e->information() = e->information() * mfgSettings->getBaWeightLine();
                e->segpts = views[fid].idealLines[lid].lsEndpoints;

                if (mfgSettings->getBaUseKernel())
                {
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    rk->setDelta(mfgSettings->getBaKernelDeltaLine());
                    e->setRobustKernel(rk);
                }

                optimizer.addEdge(e);
                vecEdgeLine.push_back(e);
            }
        }
    }


    // ---- primary planes ----
    for (int i = 0; i < plIdx2Opt.size(); ++i)
    {
        int plGid = plIdx2Opt[i];

        // --- point to plane dists ---
        for (int j = 0; j < primaryPlanes[plGid].kptGids.size(); ++j)
        {
            int ptGid = primaryPlanes[plGid].kptGids[j];

            if (ptgid2vid.find(ptGid) == ptgid2vid.end()) continue;

            g2o::EdgePointPlane3d *e = new g2o::EdgePointPlane3d();
            e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(ptgid2vid[ptGid])->second);

            if (e->vertices()[0] == 0)
            {
                cerr << "no kpt vert found ... terminated \n";
                exit(0);
            }

            e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(plgid2vid[plGid])->second);

            if (e->vertices()[1] == 0)
            {
                cerr << "no plane vert found ... terminated \n";
                exit(0);
            }

            e->setMeasurement(0);
            e->information().setIdentity();
            e->information() = mfgSettings->getBaWeightPlane() * e->information(); // observaion info

            if (mfgSettings->getBaUseKernel())
            {
                g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                rk->setDelta(mfgSettings->getBaKernelDeltaPlane());
                e->setRobustKernel(rk);
            }

            optimizer.addEdge(e);
            vecEdgePointPlane.push_back(e);
        }

        // --- line to plane dists ---
        for (int j = 0; j < primaryPlanes[plGid].ilnGids.size(); ++j)
        {
            int lnGid = primaryPlanes[plGid].ilnGids[j];

            if (lngid2vid.find(lnGid) == lngid2vid.end()) continue;

            g2o::EdgeLineVpPlane *e = new g2o::EdgeLineVpPlane();
            e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(lngid2vid[lnGid])->second);

            if (e->vertices()[0] == 0)
            {
                cerr << "no line vertex found ... terminated \n";
                exit(0);
            }

            e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(vpgid2vid[idealLines[lnGid].vpGid])->second);

            if (e->vertices()[1] == 0)
            {
                cerr << "no vp vertex found ... terminated \n";
                exit(0);
            }

            e->vertices()[2] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(plgid2vid[plGid])->second);

            if (e->vertices()[2] == 0)
            {
                cerr << "no plane vert found ... terminated \n";
                exit(0);
            }

            e->setMeasurement(0);
            e->information().setIdentity();
            e->information() = e->information() * mfgSettings->getBaWeightPlane(); // observaion info
            e->endptA = idealLines[lnGid].extremity1();
            e->endptB = idealLines[lnGid].extremity2();

            if (mfgSettings->getBaUseKernel())
            {
                g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                rk->setDelta(mfgSettings->getBaKernelDeltaPlane()*sqrt(2.0));
                e->setRobustKernel(rk);
            }

            optimizer.addEdge(e);
            vecEdgeLinePlane.push_back(e);
            e->computeError();
            Vector3d rho;
            e->robustKernel()->robustify(e->chi2(), rho);
        }
    }


    // -----------------      start g2o       -----------------//
    MyTimer timer;
    timer.start();
    optimizer.initializeOptimization();
    optimizer.computeActiveErrors();
    double baerr = optimizer.activeRobustChi2();

    if (edges_camdist.size() > 0)
    {
        for (int opt_i = 0; opt_i < 5; ++opt_i)
        {
            optimizer.optimize(maxIters);

            for (int i = 0; i < edges_camdist.size(); ++i)
                edges_camdist[i]->information() = edges_camdist[i]->information() * 100;
        }
    }
    else
        optimizer.optimize(maxIters);

    timer.end();
    cout << "LBA time:" << timer.time_ms << " ms,";

   // -------------- update camera and structure parameters  (update vertex) --------------//
    mfg_writing = true;

    double scale = 1;
    bool   toScale = false;

    for (int i = 0; i < camvertVec.size(); ++i)
    {
        if (camvertVec[i]->fixed()) continue;

        if (camvid2fid.find(camvertVec[i]->id()) == camvid2fid.end())
            cout << camvertVec[i]->id() << " vert not found ...\n";

        Vector3d t = camvertVec[i]->estimate().inverse().translation();
        Quaterniond q(camvertVec[i]->estimate().inverse().rotation());
        double qd[] = {q.w(), q.x(), q.y(), q.z()};
        views[camvid2fid[camvertVec[i]->id()]].R = q2r(qd);
        views[camvid2fid[camvertVec[i]->id()]].t = (cv::Mat_<double>(3, 1) << t(0), t(1), t(2));

        if (camvid2fid[camvertVec[i]->id()] == 1)   // second keyframe
        {
            toScale = true;
            scale = 1 / t.norm();
        }
    }

  // -------------- update structure parameters   (update vertex) --------------//
    for (int i = 0; i < ptvertVec.size(); ++i)
    {
        if (ptvertVec[i]->fixed()) continue;

        int ptGid = -1;

        if (ptvid2gid.find(ptvertVec[i]->id()) == ptvid2gid.end())
            cout << "kpt not found ......\n";
        else
            ptGid = ptvid2gid[ptvertVec[i]->id()];

        Vector3d kpt = ptvertVec[i]->estimate();
        keyPoints[ptGid].x = kpt(0);
        keyPoints[ptGid].y = kpt(1);
        keyPoints[ptGid].z = kpt(2);
    }

    for (int i = 0; i < vpvertVec.size(); ++i)
    {
        if (vpvertVec[i]->fixed()) continue;

        int vpGid = -1;

        if (vpvid2gid.find(vpvertVec[i]->id()) == vpvid2gid.end())
            cout << "vpt not found ......\n";
        else
            vpGid = vpvid2gid[vpvertVec[i]->id()];

        vanishingPoints[vpGid].x = vpvertVec[i]->estimate()(0);
        vanishingPoints[vpGid].y = vpvertVec[i]->estimate()(1);
        vanishingPoints[vpGid].z = vpvertVec[i]->estimate()(2);
    }

    for (int i = 0; i < lnvertVec.size(); ++i)
    {
        if (lnvertVec[i]->fixed()) continue;

        int lnGid = -1;

        if (lnvid2gid.find(lnvertVec[i]->id()) == lnvid2gid.end())
            cout << "line not found ......\n";
        else
            lnGid = lnvid2gid[lnvertVec[i]->id()];

        idealLines[lnGid].midpt.x = lnvertVec[i]->estimate()(0);
        idealLines[lnGid].midpt.y = lnvertVec[i]->estimate()(1);
        idealLines[lnGid].midpt.z = lnvertVec[i]->estimate()(2);
        cv::Mat vp = vanishingPoints[idealLines[lnGid].vpGid].mat(0);
        idealLines[lnGid].direct = vp / cv::norm(vp);
        cv::Point3d oldMidPt = idealLines[lnGid].midpt;
        idealLines[lnGid].midpt = projectPt3d2Ln3d(idealLines[lnGid], oldMidPt);
    }

    for (int i = 0; i < plvertVec.size(); ++i)
    {
        if (plvertVec[i]->fixed()) continue;

        int plGid = -1;

        if (plvid2gid.find(plvertVec[i]->id()) == plvid2gid.end())
        {
        }
        else
            plGid = plvid2gid[plvertVec[i]->id()];

        primaryPlanes[plGid].d = 1 / plvertVec[i]->estimate().norm();
        Vector3d n = plvertVec[i]->estimate() / plvertVec[i]->estimate().norm();
        primaryPlanes[plGid].n = (cv::Mat_<double>(3, 1) << n(0), n(1), n(2));
    }

    mfg_writing = false;
    // ----------------- error (update edges) -----------------//
    double errKpt = 0, errVnpt = 0, errLine = 0, errPlane = 0;

    for (int i = 0; i < vecEdgeKpt.size(); ++i)
    {
        if (!vecEdgeKpt[i]->allVerticesFixed())
        {
            vecEdgeKpt[i]->computeError();
            Vector3d rho;
            vecEdgeKpt[i]->robustKernel()->robustify(vecEdgeKpt[i]->chi2(), rho);
            errKpt += rho[0];
#ifdef PLOT_MID_RESULTS

            if (vecEdgeKpt[i]->chi2() > 100)
            {
                int camid = camvid2fid[vecEdgeKpt[i]->vertices()[1]->id()];
                int ptgid = ptvid2gid[vecEdgeKpt[i]->vertices()[0]->id()];

                for (int j = 0; j < keyPoints[ptgid].viewId_ptLid.size(); ++j)
                {
                    if (keyPoints[ptgid].viewId_ptLid[j][0] >= frontFrmIdx)
                    {
                        int fid = keyPoints[ptgid].viewId_ptLid[j][0];

                        if (!views[fid].matchable) continue;

                        cv::Mat canv = views[fid].img.clone();
                        cv::Point2d rpj = mat2cvpt(K * (views[fid].R * cvpt2mat(keyPoints[ptgid].cvpt(), 0) + views[fid].t));
                        cv::circle(canv, rpj, 2, cv::Scalar(0, 0, 255, 0), 2);
                        int lid = keyPoints[ptgid].viewId_ptLid[j][1];

                        if (fid == camid)
                            cv::circle(canv, views[fid].featurePoints[lid].cvpt(), 2, cv::Scalar(0, 0, 0, 0), 2);
                        else
                            cv::circle(canv, views[fid].featurePoints[lid].cvpt(), 2, cv::Scalar(200, 0, 0, 0), 2);

                        //			showImage("after ba repj "+ num2str( cv::norm(rpj-views[fid].featurePoints[lid].cvpt()) ),&canv);
                        //			cv::waitKey();
                    }
                }
            }

#endif
        }
    }

    for (int i = 0; i < vecEdgeVnpt.size(); ++i)
    {
        if (!vecEdgeVnpt[i]->allVerticesFixed())
        {
            vecEdgeVnpt[i]->computeError();
            Vector3d rho;
            vecEdgeVnpt[i]->robustKernel()->robustify(vecEdgeVnpt[i]->chi2(), rho);
            errVnpt += rho[0];
#ifdef PLOT_MID_RESULTS

            if (rho[0] > 1000)
            {
                int vpgid = vpvid2gid[vecEdgeVnpt[i]->vertices()[0]->id()];
                int camid = camvid2fid[vecEdgeVnpt[i]->vertices()[1]->id()];

                for (int j = 0; j < vanishingPoints[vpgid].viewId_vpLid.size(); ++j)
                {
                    if (camid == vanishingPoints[vpgid].viewId_vpLid[j][0])
                    {
                        cv::Mat vp3d_n = views[camid].R * vanishingPoints[vpgid].mat(0);
                        int lid = vanishingPoints[vpgid].viewId_vpLid[j][1];
                        cv::Mat vp_n = K.inv() * views[camid].vanishPoints[lid].mat();
                        vp_n = vp_n / cv::norm(vp_n);
                        double a1, b1;
                        unitVec2angle(vp3d_n, &a1, &b1);
                        double a2, b2;
                        unitVec2angle(vp_n, &a2, &b2);
                    }
                }
            }

#endif
        }
    }

    for (int i = 0; i < vecEdgeLine.size(); ++i)
    {
        if (!vecEdgeLine[i]->allVerticesFixed())
        {
            vecEdgeLine[i]->computeError();
            Vector3d rho;
            vecEdgeLine[i]->robustKernel()->robustify(vecEdgeLine[i]->chi2(), rho);
            errLine += rho[0];
#ifdef PLOT_MID_RESULTS

            if (vecEdgeLine[i]->chi2() > 100)
            {
                int lngid = lnvid2gid[vecEdgeLine[i]->vertices()[0]->id()];

                for (int j = 0; j < idealLines[lngid].viewId_lnLid.size(); ++j)
                {
                    if (idealLines[lngid].viewId_lnLid[j][0] >= frontFrmIdx)
                    {
                        int fid = idealLines[lngid].viewId_lnLid[j][0];

                        if (!views[fid].matchable) continue;

                        cv::Mat canv = views[fid].img.clone();
                        cv::Point2d ep1 = mat2cvpt(K * (views[fid].R * cvpt2mat(idealLines[lngid].extremity1(), 0) + views[fid].t));
                        cv::Point2d ep2 = mat2cvpt(K * (views[fid].R * cvpt2mat(idealLines[lngid].extremity2(), 0) + views[fid].t));
                        cv::line(canv, ep1, ep2, cv::Scalar(0, 0, 0, 0), 1);

                        int lid = idealLines[lngid].viewId_lnLid[j][1];

                        for (int k = 0; k < views[fid].idealLines[lid].lsEndpoints.size(); k = k + 2)
                        {
                            cv::line(canv, views[fid].idealLines[lid].lsEndpoints[k], views[fid].idealLines[lid].lsEndpoints[k + 1],
                                     cv::Scalar(200, 100, 1, 0), 3);
                        }
                    }
                }

            }

#endif
        }
    }

    for (int i = 0; i < vecEdgePointPlane.size(); ++i)
    {
        if (!vecEdgePointPlane[i]->allVerticesFixed())
        {
            vecEdgePointPlane[i]->computeError();
            Vector3d rho;
            vecEdgePointPlane[i]->robustKernel()->robustify(vecEdgePointPlane[i]->chi2(), rho);
            errPlane += rho[0];
        }
    }

    for (int i = 0; i < vecEdgeLinePlane.size(); ++i)
    {
        if (!vecEdgeLinePlane[i]->allVerticesFixed())
        {
            vecEdgeLinePlane[i]->computeError();
            Vector3d rho;
            vecEdgeLinePlane[i]->robustKernel()->robustify(vecEdgeLinePlane[i]->chi2(), rho);
            errPlane += rho[0];
        }
    }

    optimizer.computeActiveErrors();
    cout << "error: " << baerr << " => " << optimizer.activeRobustChi2()
         << " ( " << errKpt << " + " << errLine << " + " << errVnpt << " + " << errPlane << " )" << endl;

    optimizer.clear(); //this releases the memory of all vertices and edges in the graph   
}

void Mfg::adjustBundle_G2O(int numPos, int numFrm)
// local bundle adjustment: points+vp
{
    // Note: numFrm should be larger or equal to numPos+2, to fix scale

    // ----------------- G2O parameter setting -----------------//
    int maxIters = 25;
    // some handy typedefs
    typedef g2o::BlockSolver< g2o::BlockSolverTraits<Eigen::Dynamic, Eigen::Dynamic> >  MyBlockSolver;
    typedef g2o::LinearSolverCSparse<MyBlockSolver::PoseMatrixType> MyLinearSolver;
    typedef g2o::LinearSolverDense<MyBlockSolver::PoseMatrixType> MyDenseLinearSolver;

    // setup the solver
    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(false);
    MyLinearSolver *linearSolver = new MyLinearSolver();
    MyBlockSolver *solver_ptr = new MyBlockSolver(linearSolver);
    g2o::OptimizationAlgorithmLevenberg *solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    // -- add the parameter representing the sensor offset  !!!!
    g2o::ParameterSE3Offset *sensorOffset = new g2o::ParameterSE3Offset;
    sensorOffset->setOffset(Eigen::Isometry3d::Identity());
    sensorOffset->setId(0);
    optimizer.addParameter(sensorOffset);

    // ----- set g2o vertices ------
    int vertex_id = 0;
    int frontPosIdx = max(1, (int)views.size() - numPos);
    int frontFrmIdx = max(0, (int)views.size() - numFrm);
    int frontVptIdx = max(0, (int)views.size() - mfgSettings->getBaNumFramesVPoint()); // first frame used for keep VP estimate consistent
    frontVptIdx = min(frontFrmIdx, frontVptIdx);

//	bundle_adjust_between(frontVptIdx, views.back().id, frontPosIdx);
//	return;


    // -----------------   optimization parameters (1)~(2)  -----------------//
    // (1) add g2o vertices(camera pose parameters) to optimizer  
    vector<g2o::VertexCam *> camvertVec;           /// record every VertexCam to a vector
    unordered_map<int, int> camvid2fid, camfid2vid;/// record mapping between vertex_id and frame_id(view_from ~ view_to)

    for (int i = frontVptIdx; i < views.size(); ++i)
    {
        Eigen:: Quaterniond q = r2q(views[i].R);
        Eigen::Isometry3d pose;
        pose = q;
        pose.translation() = Eigen::Vector3d(views[i].t.at<double>(0), views[i].t.at<double>(1), views[i].t.at<double>(2));
        g2o::VertexCam *v_cam = new g2o::VertexCam();
        v_cam->setId(vertex_id);
        g2o::SBACam sc(q.inverse(), pose.inverse().translation());
        sc.setKcam(K.at<double>(0, 0), K.at<double>(1, 1), K.at<double>(0, 2), K.at<double>(1, 2), 0);
        v_cam->setEstimate(sc);

        if (i < 1 || i < frontPosIdx)
            v_cam->setFixed(true);

        optimizer.addVertex(v_cam);
        camvid2fid[vertex_id] = i;
        camfid2vid[i] = vertex_id;
        ++vertex_id;
        camvertVec.push_back(v_cam);
    }

    // (2) add g2o edges(camera to camera distance constraints) to optimizer 
    vector<g2o::EdgeCamCamDist *> edges_camdist; /// record every EdgeCamCamDist to a vector

    for (int i = camdist_constraints.size() - 1; i >= 0 && i >= (int)camdist_constraints.size() - numFrm; --i)
    {
        if (camfid2vid.find((int)camdist_constraints[i][0]) != camfid2vid.end() &&
                camfid2vid.find((int)camdist_constraints[i][1]) != camfid2vid.end()
           )   // apply (hard) contraints
        {
            maxIters = 20;
            //  optimizer.setVerbose(true);
            g2o::EdgeCamCamDist *e = new g2o::EdgeCamCamDist();
            e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(camfid2vid[(int)camdist_constraints[i][0]])->second);

            if (e->vertices()[0] == 0)
            {
                cerr << "no cam vertex found ... terminated \n";
                exit(0);
            }

            e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(camfid2vid[(int)camdist_constraints[i][1]])->second);

            if (e->vertices()[1] == 0)
            {
                cerr << "no cam vertex found ... terminated \n";
                exit(0);
            }

            e->setMeasurement(camdist_constraints[i][2]);
            e->information().setIdentity();
            e->information() = e->information() * camdist_constraints[i][3]; // observaion info
            optimizer.addEdge(e);
            edges_camdist.push_back(e);
        }
    }

    if (edges_camdist.size() > 0)
    {
        for (int i = 0; i < camvertVec.size(); ++i)
        {
            if (i > 0)
                camvertVec[i]->setFixed(false); //scale already fixed by dist-constaint, only need to fix one front camera
        }
    }

    // -----------------   structure parameters (1)~(2)  -----------------//
	// (1) add g2o vertices (keypoints, vanishpoints, ideal lines, primary planes) to optimizer  
    // ---- keypoints ----
    unordered_map<int, int> ptvid2gid, ptgid2vid;
    vector<g2o::VertexSBAPointXYZ *> ptvertVec;
    vector<int> kptIdx2Opt;        // keyPoint idx to optimize
    vector<int> kptIdx2Rpj_notOpt; // keypoint idx to reproject but not optimize

    // points-to-optimize contains those first appearing after frontFrmIdx and still being observed after frontPosIdx
    for (int i = 0; i < keyPoints.size(); ++i)
    {
        if (!keyPoints[i].is3D || keyPoints[i].gid < 0) continue;

        for (int j = 0; j < keyPoints[i].viewId_ptLid.size(); ++j)
        {
            if (keyPoints[i].viewId_ptLid[j][0] >= frontPosIdx)
            {
                // don't optimize too-old (established before frontFrmIdx) points,
                // but still use their recent observations/reprojections after frontPosIdx
                g2o::VertexSBAPointXYZ *v_p = new g2o::VertexSBAPointXYZ();
                v_p->setId(vertex_id);
                v_p->setMarginalized(true);
                Eigen::Vector3d pt(keyPoints[i].x, keyPoints[i].y, keyPoints[i].z);
                v_p->setEstimate(pt);

                if (keyPoints[i].viewId_ptLid[0][0] < frontFrmIdx
                        && keyPoints[i].estViewId < views.back().id
                        && edges_camdist.size() == 0 //
                   )
                {
                    kptIdx2Rpj_notOpt.push_back(i);
                    v_p->setFixed(true);
                }
                else     // not fixed, to be optimized
                {
                    v_p->setFixed(false);
                    kptIdx2Opt.push_back(i);
                    ptvertVec.push_back(v_p);
                }

                optimizer.addVertex(v_p);
                ptgid2vid[keyPoints[i].gid] = vertex_id;
                ptvid2gid[vertex_id] = keyPoints[i].gid;
                ++vertex_id;
                break;
            }
        }
    }

    // ---- vanishing points ----
    unordered_map<int, int> vpgid2vid, vpvid2gid;
    vector<int> vpIdx2Opt;
    vector<g2o::VertexVanishPoint *> vpvertVec;

    // vp that is observed in current window
    for (int i = 0; i < vanishingPoints.size(); ++i)
    {
        for (int j = 0; j < vanishingPoints[i].viewId_vpLid.size(); ++j)
        {
            if (vanishingPoints[i].viewId_vpLid[j][0] >= frontPosIdx)  // observed recently
            {
                vpIdx2Opt.push_back(i);
                g2o::VertexVanishPoint *v_vp = new g2o::VertexVanishPoint();
                v_vp->setId(vertex_id);
                Eigen::Vector3d pt(vanishingPoints[i].x, vanishingPoints[i].y, vanishingPoints[i].z);
                v_vp->setEstimate(pt);
                optimizer.addVertex(v_vp);
                vpvertVec.push_back(v_vp);
                vpvid2gid[vertex_id] = vanishingPoints[i].gid;
                vpgid2vid[vanishingPoints[i].gid] = vertex_id;
                ++vertex_id;
                break;
            }
        }
    }

    // ---- ideal lines ----
    unordered_map<int, int> lngid2vid, lnvid2gid;
    vector<int> lnIdx2Opt, lnIdx2Rpj_notOpt;
    vector<g2o::VertexSBAPointXYZ *> lnvertVec;

    for (int i = 0; i < idealLines.size(); ++i)
    {
        if (!idealLines[i].is3D || idealLines[i].gid < 0) continue;

        for (int j = 0; j < idealLines[i].viewId_lnLid.size(); ++j)
        {
            if (idealLines[i].viewId_lnLid[j][0] >= frontPosIdx)
            {
                g2o::VertexSBAPointXYZ *v_lnpt = new g2o::VertexSBAPointXYZ();
                Vector3d pt(idealLines[i].midpt.x, idealLines[i].midpt.y, idealLines[i].midpt.z);
                v_lnpt->setEstimate(pt);
                v_lnpt->setId(vertex_id);
                lngid2vid[idealLines[i].gid] = vertex_id;
                lnvid2gid[vertex_id] = idealLines[i].gid;

                if (idealLines[i].viewId_lnLid[0][0] < frontFrmIdx
                        && idealLines[i].estViewId < views.back().id)
                {
                    lnIdx2Rpj_notOpt.push_back(i);
                    v_lnpt->setFixed(true);
                }
                else
                {
                    lnIdx2Opt.push_back(i);
                    v_lnpt->setFixed(false);
                    lnvertVec.push_back(v_lnpt);
                }

                optimizer.addVertex(v_lnpt);
                ++vertex_id;
                break;
            }
        }
    }

    // ---- primary planes ----
    unordered_map<int, int> plgid2vid, plvid2gid;
    vector<int> plIdx2Opt;
    vector<g2o::VertexPlane3d *> plvertVec;

    for (int i = 0; i < primaryPlanes.size(); ++i)
    {
        if (views.back().id - primaryPlanes[i].estViewId < 3) continue;

        bool usePlane = false;

        for (int j = 0; j < primaryPlanes[i].kptGids.size(); ++j)
        {
            if (ptgid2vid.find(primaryPlanes[i].kptGids[j]) != ptgid2vid.end())
            {
                usePlane = true;
                break;
            }
        }

        for (int j = 0; j < primaryPlanes[i].ilnGids.size(); ++j)
        {
            if (lngid2vid.find(primaryPlanes[i].ilnGids[j]) != lngid2vid.end())
            {
                usePlane = true;
                break;
            }
        }

        if (!usePlane) continue;

        g2o::VertexPlane3d *v_pl = new g2o::VertexPlane3d();
        v_pl->setId(vertex_id);
        Eigen::Vector3d pl(primaryPlanes[i].n.at<double>(0) / primaryPlanes[i].d,
                           primaryPlanes[i].n.at<double>(1) / primaryPlanes[i].d,
                           primaryPlanes[i].n.at<double>(2) / primaryPlanes[i].d);
        v_pl->setEstimate(pl);

        if (primaryPlanes[i].estViewId < frontPosIdx)
            v_pl->setFixed(true); // don't change old planes
        else
            v_pl->setFixed(false);

        optimizer.addVertex(v_pl);
        plgid2vid[i] = vertex_id;
        plvid2gid[vertex_id] = i;
        plIdx2Opt.push_back(i);
        plvertVec.push_back(v_pl);
        ++vertex_id;
    }

// (2) add g2o edges (keypoints, vanishpoints, lines, primary planes) to optimizer  
    vector<g2o::EdgeVnptCam *>      vecEdgeVnpt;
    vector<g2o::EdgeProjectP2MC *>  vecEdgeKpt;
    vector<g2o::EdgeLineVpCam *>    vecEdgeLine;
    vector<g2o::EdgePointPlane3d *> vecEdgePointPlane;
    vector<g2o::EdgeLineVpPlane *>  vecEdgeLinePlane;

    // ---- keypoints ----
    for (int i = 0; i < kptIdx2Opt.size(); ++i)// keyPoint idx to optimize
    {
        for (int j = 0; j < keyPoints[kptIdx2Opt[i]].viewId_ptLid.size(); ++j)
        {
            int fid = keyPoints[kptIdx2Opt[i]].viewId_ptLid[j][0]; //fram(view) id
            int lid = keyPoints[kptIdx2Opt[i]].viewId_ptLid[j][1];//local id

            if (!views[fid].matchable) continue;

            if (fid >= frontFrmIdx)
            {
                g2o::EdgeProjectP2MC *e = new g2o::EdgeProjectP2MC();
                e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(ptgid2vid[kptIdx2Opt[i]])->second);

                if (e->vertices()[0] == 0)
                {
                    cerr << "no pt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(camfid2vid[fid])->second);

                if (e->vertices()[1] == 0)
                {
                    cerr << "no cam vert found ... terminated \n";
                    exit(0);
                }

                Eigen::Vector2d meas(views[fid].featurePoints[lid].x, views[fid].featurePoints[lid].y);
                e->setMeasurement(meas);

                if (mfgSettings->getBaUseKernel())
                {
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    rk->setDelta(mfgSettings->getBaKernelDeltaPoint());
                    e->setRobustKernel(rk);
                }

                e->information() = Matrix2d::Identity();
                optimizer.addEdge(e);
                vecEdgeKpt.push_back(e);
            }
        }
    }

    for (int i = 0; i < kptIdx2Rpj_notOpt.size(); ++i)// keypoint idx to reproject but not optimize
    {
        for (int j = 0; j < keyPoints[kptIdx2Rpj_notOpt[i]].viewId_ptLid.size(); ++j)
        {
            int fid = keyPoints[kptIdx2Rpj_notOpt[i]].viewId_ptLid[j][0] ;
            int lid = keyPoints[kptIdx2Rpj_notOpt[i]].viewId_ptLid[j][1] ;

            if (!views[fid].matchable) continue;

            if (fid >= frontFrmIdx)
            {
                g2o::EdgeProjectP2MC *e = new g2o::EdgeProjectP2MC();
                e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(ptgid2vid[kptIdx2Rpj_notOpt[i]])->second);

                if (e->vertices()[0] == 0)
                {
                    cerr << "no pt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(camfid2vid[fid])->second);

                if (e->vertices()[1] == 0)
                {
                    cerr << "no cam vert found ... terminated \n";
                    exit(0);
                }

                Eigen::Vector2d meas(views[fid].featurePoints[lid].x, views[fid].featurePoints[lid].y);
                e->setMeasurement(meas);
                e->information() = Matrix2d::Identity();

                if (mfgSettings->getBaUseKernel())
                {
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    rk->setDelta(mfgSettings->getBaKernelDeltaPoint());
                    e->setRobustKernel(rk);
                }

                optimizer.addEdge(e);
                vecEdgeKpt.push_back(e);
            }
        }
    }

    // ---- vanishpoints ----
    for (int i = 0; i < vpIdx2Opt.size(); ++i)
    {
        int vpGid = vpIdx2Opt[i];

        for (int j = 0; j < vanishingPoints[vpGid].viewId_vpLid.size(); ++j)
        {
            int fid = vanishingPoints[vpGid].viewId_vpLid[j][0];
            int lid = vanishingPoints[vpGid].viewId_vpLid[j][1];

            if (!views[fid].matchable) continue;

            if (fid >= frontVptIdx)  // different from frontFrmIdx
            {
                g2o::EdgeVnptCam *e = new g2o::EdgeVnptCam();
                e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(vpgid2vid[vpGid])->second);

                if (e->vertices()[0] == 0)
                {
                    cerr << "no vpt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(camfid2vid[fid])->second);

                if (e->vertices()[1] == 0)
                {
                    cerr << "no cam vert found ... terminated \n";
                    exit(0);
                }

                cv::Mat univec_meas = K.inv() * views[fid].vanishPoints[lid].mat();
                univec_meas = univec_meas / cv::norm(univec_meas);
                double alpha, beta;
                unitVec2angle(univec_meas, &alpha, &beta);
                Eigen::Vector2d meas(alpha, beta);
                e->setMeasurement(meas);
                e->information() = Matrix2d::Identity();
                e->computeError();
                double err_raw = e->chi2();
                cv::Mat egvals;
                cv::eigen(views[fid].vanishPoints[lid].cov_pt(K), egvals);
                Matrix2d cov;
                cov(0, 0) = max(views[fid].vanishPoints[lid].cov_ab.at<double>(0, 0), 1e-3);
                cov(0, 1) = views[fid].vanishPoints[lid].cov_ab.at<double>(0, 1) * 0;
                cov(1, 0) = views[fid].vanishPoints[lid].cov_ab.at<double>(1, 0) * 0;
                cov(1, 1) = max(views[fid].vanishPoints[lid].cov_ab.at<double>(1, 1), 1e-3);
                e->information() = mfgSettings->getBaWeightVPoint() * cov.inverse(); // observaion info

                if (mfgSettings->getBaUseKernel())
                {
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    rk->setDelta(mfgSettings->getBaKernelDeltaVPoint());
                    e->setRobustKernel(rk);
                }

                optimizer.addEdge(e);
                vecEdgeVnpt.push_back(e);
            }
        }
    }

    // ---- lines ----
    for (int i = 0; i < lnIdx2Opt.size(); ++i)// lnIdx2Opt idx to optimize 
    {
        for (int j = 0; j < idealLines[lnIdx2Opt[i]].viewId_lnLid.size(); ++j)
        {
            int fid = idealLines[lnIdx2Opt[i]].viewId_lnLid[j][0];
            int lid = idealLines[lnIdx2Opt[i]].viewId_lnLid[j][1];

            if (!views[fid].matchable) continue;

            if (fid >= frontFrmIdx)
            {
                g2o::EdgeLineVpCam *e = new g2o::EdgeLineVpCam();
                e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(lngid2vid[lnIdx2Opt[i]])->second);

                if (e->vertices()[0] == 0)
                {
                    cerr << "no lnpt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(vpgid2vid[idealLines[lnIdx2Opt[i]].vpGid])->second);

                if (e->vertices()[1] == 0)
                {
                    cerr << "no vpt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[2] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(camfid2vid[fid])->second);

                if (e->vertices()[2] == 0)
                {
                    cerr << "no cam vert found ... terminated \n";
                    exit(0);
                }

                double meas = 0;
                e->setMeasurement(meas);
                e->information() = e->information() * mfgSettings->getBaWeightLine();

                if (mfgSettings->getBaUseKernel())
                {
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    rk->setDelta(mfgSettings->getBaKernelDeltaLine());
                    e->setRobustKernel(rk);
                }

                e->segpts = views[fid].idealLines[lid].lsEndpoints;
                optimizer.addEdge(e);
                vecEdgeLine.push_back(e);
            }
        }
    }

    for (int i = 0; i < lnIdx2Rpj_notOpt.size(); ++i)// lnIdx2Rpj_notOpt idx to reproject but not optimize
    {
        for (int j = 0; j < idealLines[lnIdx2Rpj_notOpt[i]].viewId_lnLid.size(); ++j)
        {
            int fid = idealLines[lnIdx2Rpj_notOpt[i]].viewId_lnLid[j][0];
            int lid = idealLines[lnIdx2Rpj_notOpt[i]].viewId_lnLid[j][1];

            if (!views[fid].matchable) continue;

            if (fid >= frontFrmIdx)
            {
                g2o::EdgeLineVpCam *e = new g2o::EdgeLineVpCam();
                e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(lngid2vid[lnIdx2Rpj_notOpt[i]])->second);

                if (e->vertices()[0] == 0)
                {
                    cerr << "no lnpt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(vpgid2vid[idealLines[lnIdx2Rpj_notOpt[i]].vpGid])->second);

                if (e->vertices()[1] == 0)
                {
                    cerr << "no vpt vert found ... terminated \n";
                    exit(0);
                }

                e->vertices()[2] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                                   (optimizer.vertices().find(camfid2vid[fid])->second);

                if (e->vertices()[2] == 0)
                {
                    cerr << "no cam vert found ... terminated \n";
                    exit(0);
                }

                double meas = 0;
                e->setMeasurement(meas);
                e->information() = e->information() * mfgSettings->getBaWeightLine();
                e->segpts = views[fid].idealLines[lid].lsEndpoints;

                if (mfgSettings->getBaUseKernel())
                {
                    g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                    rk->setDelta(mfgSettings->getBaKernelDeltaLine());
                    e->setRobustKernel(rk);
                }

                optimizer.addEdge(e);
                vecEdgeLine.push_back(e);
            }
        }
    }


    // ---- primary planes ----
    for (int i = 0; i < plIdx2Opt.size(); ++i)
    {
        int plGid = plIdx2Opt[i];

        // --- point to plane dists ---
        for (int j = 0; j < primaryPlanes[plGid].kptGids.size(); ++j)
        {
            int ptGid = primaryPlanes[plGid].kptGids[j];

            if (ptgid2vid.find(ptGid) == ptgid2vid.end()) continue;

            g2o::EdgePointPlane3d *e = new g2o::EdgePointPlane3d();
            e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(ptgid2vid[ptGid])->second);

            if (e->vertices()[0] == 0)
            {
                cerr << "no kpt vert found ... terminated \n";
                exit(0);
            }

            e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(plgid2vid[plGid])->second);

            if (e->vertices()[1] == 0)
            {
                cerr << "no plane vert found ... terminated \n";
                exit(0);
            }

            e->setMeasurement(0);
            e->information().setIdentity();
            e->information() = mfgSettings->getBaWeightPlane() * e->information(); // observaion info

            if (mfgSettings->getBaUseKernel())
            {
                g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                rk->setDelta(mfgSettings->getBaKernelDeltaPlane());
                e->setRobustKernel(rk);
            }

            optimizer.addEdge(e);
            vecEdgePointPlane.push_back(e);
        }

        // --- line to plane dists ---
        for (int j = 0; j < primaryPlanes[plGid].ilnGids.size(); ++j)
        {
            int lnGid = primaryPlanes[plGid].ilnGids[j];

            if (lngid2vid.find(lnGid) == lngid2vid.end()) continue;

            g2o::EdgeLineVpPlane *e = new g2o::EdgeLineVpPlane();
            e->vertices()[0] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(lngid2vid[lnGid])->second);

            if (e->vertices()[0] == 0)
            {
                cerr << "no line vertex found ... terminated \n";
                exit(0);
            }

            e->vertices()[1] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(vpgid2vid[idealLines[lnGid].vpGid])->second);

            if (e->vertices()[1] == 0)
            {
                cerr << "no vp vertex found ... terminated \n";
                exit(0);
            }

            e->vertices()[2] = dynamic_cast<g2o::OptimizableGraph::Vertex *>
                               (optimizer.vertices().find(plgid2vid[plGid])->second);

            if (e->vertices()[2] == 0)
            {
                cerr << "no plane vert found ... terminated \n";
                exit(0);
            }

            e->setMeasurement(0);
            e->information().setIdentity();
            e->information() = e->information() * mfgSettings->getBaWeightPlane(); // observaion info
            e->endptA = idealLines[lnGid].extremity1();
            e->endptB = idealLines[lnGid].extremity2();

            if (mfgSettings->getBaUseKernel())
            {
                g2o::RobustKernelHuber *rk = new g2o::RobustKernelHuber;
                rk->setDelta(mfgSettings->getBaKernelDeltaPlane()*sqrt(2.0));
                e->setRobustKernel(rk);
            }

            optimizer.addEdge(e);
            vecEdgeLinePlane.push_back(e);
            e->computeError();
            Vector3d rho;
            e->robustKernel()->robustify(e->chi2(), rho);
        }
    }


    // -----------------      start g2o       -----------------//
    MyTimer timer;
    timer.start();
    optimizer.initializeOptimization();
    optimizer.computeActiveErrors();
    double baerr = optimizer.activeRobustChi2();

    if (edges_camdist.size() > 0)
    {
        for (int opt_i = 0; opt_i < 5; ++opt_i)
        {
            optimizer.optimize(maxIters);

            for (int i = 0; i < edges_camdist.size(); ++i)
                edges_camdist[i]->information() = edges_camdist[i]->information() * 100;
        }
    }
    else
        optimizer.optimize(maxIters);

    timer.end();
    cout << "LBA time:" << timer.time_ms << " ms,";

 // -------------- update camera and structure parameters  (update vertex) --------------//
    mfg_writing = true;
    double scale = 1;
    bool   toScale = false;

    for (int i = 0; i < camvertVec.size(); ++i)
    {
        if (camvertVec[i]->fixed()) continue;

        if (camvid2fid.find(camvertVec[i]->id()) == camvid2fid.end())
            cout << camvertVec[i]->id() << " vert not found ...\n";

        Vector3d t = camvertVec[i]->estimate().inverse().translation();
        Quaterniond q(camvertVec[i]->estimate().inverse().rotation());
        double qd[] = {q.w(), q.x(), q.y(), q.z()};
        views[camvid2fid[camvertVec[i]->id()]].R = q2r(qd);
        views[camvid2fid[camvertVec[i]->id()]].t = (cv::Mat_<double>(3, 1) << t(0), t(1), t(2));

        if (camvid2fid[camvertVec[i]->id()] == 1)   // second keyframe
        {
            toScale = true;
            scale = 1 / t.norm();
        }
    }

  // -------------- update structure parameters   (update vertex) --------------//
    for (int i = 0; i < ptvertVec.size(); ++i)
    {
        if (ptvertVec[i]->fixed()) continue;

        int ptGid = -1;

        if (ptvid2gid.find(ptvertVec[i]->id()) == ptvid2gid.end())
            cout << "kpt not found ......\n";
        else
            ptGid = ptvid2gid[ptvertVec[i]->id()];

        Vector3d kpt = ptvertVec[i]->estimate();
        keyPoints[ptGid].x = kpt(0);
        keyPoints[ptGid].y = kpt(1);
        keyPoints[ptGid].z = kpt(2);
    }

    for (int i = 0; i < vpvertVec.size(); ++i)
    {
        if (vpvertVec[i]->fixed()) continue;

        int vpGid = -1;

        if (vpvid2gid.find(vpvertVec[i]->id()) == vpvid2gid.end())
            cout << "vpt not found ......\n";
        else
            vpGid = vpvid2gid[vpvertVec[i]->id()];

        vanishingPoints[vpGid].x = vpvertVec[i]->estimate()(0);
        vanishingPoints[vpGid].y = vpvertVec[i]->estimate()(1);
        vanishingPoints[vpGid].z = vpvertVec[i]->estimate()(2);
    }

    for (int i = 0; i < lnvertVec.size(); ++i)
    {
        if (lnvertVec[i]->fixed()) continue;

        int lnGid = -1;

        if (lnvid2gid.find(lnvertVec[i]->id()) == lnvid2gid.end())
            cout << "line not found ......\n";
        else
            lnGid = lnvid2gid[lnvertVec[i]->id()];

        idealLines[lnGid].midpt.x = lnvertVec[i]->estimate()(0);
        idealLines[lnGid].midpt.y = lnvertVec[i]->estimate()(1);
        idealLines[lnGid].midpt.z = lnvertVec[i]->estimate()(2);
        cv::Mat vp = vanishingPoints[idealLines[lnGid].vpGid].mat(0);
        idealLines[lnGid].direct = vp / cv::norm(vp);
        cv::Point3d oldMidPt = idealLines[lnGid].midpt;
        idealLines[lnGid].midpt = projectPt3d2Ln3d(idealLines[lnGid], oldMidPt);
    }

    for (int i = 0; i < plvertVec.size(); ++i)
    {
        if (plvertVec[i]->fixed()) continue;

        int plGid = -1;

        if (plvid2gid.find(plvertVec[i]->id()) == plvid2gid.end())
        {
        }
        else
            plGid = plvid2gid[plvertVec[i]->id()];

        primaryPlanes[plGid].d = 1 / plvertVec[i]->estimate().norm();
        Vector3d n = plvertVec[i]->estimate() / plvertVec[i]->estimate().norm();
        primaryPlanes[plGid].n = (cv::Mat_<double>(3, 1) << n(0), n(1), n(2));
    }

    mfg_writing = false;
    // ----------------- error (update edges) -----------------//
    double errKpt = 0, errVnpt = 0, errLine = 0, errPlane = 0;

    for (int i = 0; i < vecEdgeKpt.size(); ++i)
    {
        if (!vecEdgeKpt[i]->allVerticesFixed())
        {
            vecEdgeKpt[i]->computeError();
            Vector3d rho;
            vecEdgeKpt[i]->robustKernel()->robustify(vecEdgeKpt[i]->chi2(), rho);
            errKpt += rho[0];
#ifdef PLOT_MID_RESULTS

            if (vecEdgeKpt[i]->chi2() > 100)
            {
                int camid = camvid2fid[vecEdgeKpt[i]->vertices()[1]->id()];
                int ptgid = ptvid2gid[vecEdgeKpt[i]->vertices()[0]->id()];

                for (int j = 0; j < keyPoints[ptgid].viewId_ptLid.size(); ++j)
                {
                    if (keyPoints[ptgid].viewId_ptLid[j][0] >= frontFrmIdx)
                    {
                        int fid = keyPoints[ptgid].viewId_ptLid[j][0];

                        if (!views[fid].matchable) continue;

                        cv::Mat canv = views[fid].img.clone();
                        cv::Point2d rpj = mat2cvpt(K * (views[fid].R * cvpt2mat(keyPoints[ptgid].cvpt(), 0) + views[fid].t));
                        cv::circle(canv, rpj, 2, cv::Scalar(0, 0, 255, 0), 2);
                        int lid = keyPoints[ptgid].viewId_ptLid[j][1];

                        if (fid == camid)
                            cv::circle(canv, views[fid].featurePoints[lid].cvpt(), 2, cv::Scalar(0, 0, 0, 0), 2);
                        else
                            cv::circle(canv, views[fid].featurePoints[lid].cvpt(), 2, cv::Scalar(200, 0, 0, 0), 2);

                        //			showImage("after ba repj "+ num2str( cv::norm(rpj-views[fid].featurePoints[lid].cvpt()) ),&canv);
                        //			cv::waitKey();
                    }
                }
            }

#endif
        }
    }

    for (int i = 0; i < vecEdgeVnpt.size(); ++i)
    {
        if (!vecEdgeVnpt[i]->allVerticesFixed())
        {
            vecEdgeVnpt[i]->computeError();
            Vector3d rho;
            vecEdgeVnpt[i]->robustKernel()->robustify(vecEdgeVnpt[i]->chi2(), rho);
            errVnpt += rho[0];
#ifdef PLOT_MID_RESULTS

            if (rho[0] > 1000)
            {
                int vpgid = vpvid2gid[vecEdgeVnpt[i]->vertices()[0]->id()];
                int camid = camvid2fid[vecEdgeVnpt[i]->vertices()[1]->id()];

                for (int j = 0; j < vanishingPoints[vpgid].viewId_vpLid.size(); ++j)
                {
                    if (camid == vanishingPoints[vpgid].viewId_vpLid[j][0])
                    {
                        cv::Mat vp3d_n = views[camid].R * vanishingPoints[vpgid].mat(0);
                        int lid = vanishingPoints[vpgid].viewId_vpLid[j][1];
                        cv::Mat vp_n = K.inv() * views[camid].vanishPoints[lid].mat();
                        vp_n = vp_n / cv::norm(vp_n);
                        double a1, b1;
                        unitVec2angle(vp3d_n, &a1, &b1);
                        double a2, b2;
                        unitVec2angle(vp_n, &a2, &b2);

                    }
                }
            }

#endif
        }
    }

    for (int i = 0; i < vecEdgeLine.size(); ++i)
    {
        if (!vecEdgeLine[i]->allVerticesFixed())
        {
            vecEdgeLine[i]->computeError();
            Vector3d rho;
            vecEdgeLine[i]->robustKernel()->robustify(vecEdgeLine[i]->chi2(), rho);
            errLine += rho[0];
#ifdef PLOT_MID_RESULTS

            if (vecEdgeLine[i]->chi2() > 100)
            {
                int lngid = lnvid2gid[vecEdgeLine[i]->vertices()[0]->id()];

                for (int j = 0; j < idealLines[lngid].viewId_lnLid.size(); ++j)
                {
                    if (idealLines[lngid].viewId_lnLid[j][0] >= frontFrmIdx)
                    {
                        int fid = idealLines[lngid].viewId_lnLid[j][0];

                        if (!views[fid].matchable) continue;

                        cv::Mat canv = views[fid].img.clone();
                        cv::Point2d ep1 = mat2cvpt(K * (views[fid].R * cvpt2mat(idealLines[lngid].extremity1(), 0) + views[fid].t));
                        cv::Point2d ep2 = mat2cvpt(K * (views[fid].R * cvpt2mat(idealLines[lngid].extremity2(), 0) + views[fid].t));
                        cv::line(canv, ep1, ep2, cv::Scalar(0, 0, 0, 0), 1);

                        int lid = idealLines[lngid].viewId_lnLid[j][1];

                        for (int k = 0; k < views[fid].idealLines[lid].lsEndpoints.size(); k = k + 2)
                        {
                            cv::line(canv, views[fid].idealLines[lid].lsEndpoints[k], views[fid].idealLines[lid].lsEndpoints[k + 1],
                                     cv::Scalar(200, 100, 1, 0), 3);
                        }
                    }
                }

            }

#endif
        }
    }

    for (int i = 0; i < vecEdgePointPlane.size(); ++i)
    {
        if (!vecEdgePointPlane[i]->allVerticesFixed())
        {
            vecEdgePointPlane[i]->computeError();
            Vector3d rho;
            vecEdgePointPlane[i]->robustKernel()->robustify(vecEdgePointPlane[i]->chi2(), rho);
            errPlane += rho[0];
        }
    }

    for (int i = 0; i < vecEdgeLinePlane.size(); ++i)
    {
        if (!vecEdgeLinePlane[i]->allVerticesFixed())
        {
            vecEdgeLinePlane[i]->computeError();
            Vector3d rho;
            vecEdgeLinePlane[i]->robustKernel()->robustify(vecEdgeLinePlane[i]->chi2(), rho);
            errPlane += rho[0];
        }
    }

    optimizer.computeActiveErrors();
    cout << "error: " << baerr << " => " << optimizer.activeRobustChi2();
    //	<<" ( "<<errKpt<<" + "<<errLine << " + "<<errVnpt<<" + "<<errPlane<<" )"<<endl;
    views.back().errAll = optimizer.activeRobustChi2();
    views.back().errPt = errKpt;
    views.back().errLn = errLine;
    views.back().errPl = errPlane;

    optimizer.clear(); //this releases the memory of all vertices and edges in the graph
}
