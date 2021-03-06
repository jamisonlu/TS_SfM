#include "System.h"

#include "Frame.h"
#include "KPExtractor.h"

#include "Matcher.h"
#include "Solver.h"
#include "Optimizer.h"

#include "Reconstructor.h"
#include "Map.h"
#include "MapPoint.h"

#include "Viewer.h"

#include "Utils.h"

#include <functional>

namespace TS_SfM {
  System::System(const std::string& str_config_file) : m_config_file(str_config_file) {
    std::pair<SystemConfig, Camera> _pair_config = ConfigLoader::LoadConfig(str_config_file);  
    m_config = _pair_config.first;
    m_camera = _pair_config.second;

    ConfigLoader::LoadInitializerConfig(m_initializer_config.num_frames, m_initializer_config.connect_distance, str_config_file);
    
    m_vstr_image_names = ConfigLoader::ReadImagesInDir(m_config.str_path_to_images);
    const cv::Mat m_image = ConfigLoader::LoadImage(m_vstr_image_names[0]);

    if (m_camera.f_cx < 1.0) {
      m_camera.f_cx *= (float)m_image.rows; 
    }
    if (m_camera.f_cy < 1.0) {
      m_camera.f_cy *= (float)m_image.cols; 
    }

    m_image_width = m_image.cols;
    m_image_height = m_image.rows;

    ShowConfig();
    m_v_frames.reserve((int)m_vstr_image_names.size()); 

    for(size_t i = 0; i < m_vstr_image_names.size(); ++i) {
      Frame frame(i, m_vstr_image_names[i]); 
      m_v_frames.push_back(frame);
    }

    // Matcher::MatcherConfig m_matcher_config = ConfigLoader::LoadMatcherConfig(str_config_file);  
    m_p_extractor.reset(new KPExtractor(m_image_width, m_image_height,
                        ConfigLoader::LoadExtractorConfig(str_config_file)));

    m_p_map = std::make_shared<Map>();
    m_p_reconstructor.reset(new Reconstructor(str_config_file));
    m_p_viewer.reset(new Viewer());
  }

  System::~System() {

  };

  void System::DrawEpiLines(const Frame& f0, const Frame& f1, 
                            const std::vector<cv::DMatch>& v_matches01, const std::vector<bool>& vb_mask,
                            const cv::Mat& F) const
  {
    cv::Mat output = f1.GetImage().clone();
    cv::Mat image0 = f0.GetImage().clone();
    int max_line_num = 20;
    int line_num = 0;
    std::vector<cv::KeyPoint> vkpts0 = f0.GetKeyPoints();
    std::vector<cv::KeyPoint> vkpts1 = f1.GetKeyPoints();
    for(size_t i = 0; i < vb_mask.size(); ++i) {
      if(vb_mask[i] && line_num < max_line_num) {
        cv::Mat pt0 = (cv::Mat_<float>(3,1) << vkpts0[i].pt.x, vkpts0[i].pt.y, 1.0);
        cv::Mat l = F * cv::Mat(pt0);
        cv::Vec3f line(l.reshape(3).at<cv::Vec3f>());

        cv::line(output,
                 cv::Point2f(0, -line(2)/line(1)),
                 cv::Point2f((float)output.cols-1.0, (-line(2)-line(0)*(output.cols-1))/line(1) ), 
                 cv::Scalar(0,255,0), 1);

        cv::circle(image0, cv::Point((int)vkpts0[i].pt.x, (int)vkpts0[i].pt.y), 3,cv::Scalar(0,0,255), 2);
      
        i += 30;
        line_num++;
      } 
    }

    cv::imshow("epipolar-line", output);
    cv::imshow("image0", image0);
    cv::waitKey();


    return;
  }

  void System::InitializeFrames(std::vector<Frame>& v_frames, const int num_frames_in_initial_map)
  {
    for (int i = 0; i < num_frames_in_initial_map; i++) {
      bool isOK = false;
      m_p_extractor = v_frames[i].Initialize(std::move(m_p_extractor), isOK);
    }
    std::cout << " Done. " << std::endl;

    std::cout << "[LOG] "
              << "SfM pipeline starts ...";

#if 0
    for (size_t i = 0; i < v_frames.size(); i++) {
      // v_frames[i].ShowFeaturePoints();    
      v_frames[i].ShowFeaturePointsInGrids();    
    }
#endif

    std::cout << "[LOG] "
              << "Extracting Feature points ...";
    std::cout << " Done. " << std::endl;

    return;
  }

  // Initialization is done in 3-view geometry
  int System::InitializeGlobalMap(std::vector<std::reference_wrapper<Frame>>& v_frames) {
    int num_map_points = -1; 
    assert(v_frames.size() == 3);  

    Frame& frame_1st = v_frames[0].get();
    Frame& frame_2nd = v_frames[1].get();
    Frame& frame_3rd = v_frames[2].get();

    Matcher matcher(ConfigLoader::LoadMatcherConfig(m_config_file));

    std::vector<cv::DMatch> v_matches_12 = matcher.GetMatches(frame_1st,frame_2nd);
    std::vector<cv::DMatch> v_matches_13 = matcher.GetMatches(frame_1st,frame_3rd);
    std::vector<cv::DMatch> v_matches_23 = matcher.GetMatches(frame_2nd,frame_3rd);

    // Compute Fundamental Matrix
    cv::Mat mK = (cv::Mat_<float>(3,3) << m_camera.f_fx, 0.0, m_camera.f_cx,
                                          0.0, m_camera.f_fy, m_camera.f_cy,
                                          0.0,           0.0,           1.0);

    
    cv::Mat mF;
    std::vector<bool> vb_mask;
    int score;
    Solver::SolveEpipolarConstraintRANSAC(frame_1st.GetImage(), frame_2nd.GetImage(),  
                                          std::make_pair(frame_1st.GetKeyPoints(),frame_2nd.GetKeyPoints()),
                                          v_matches_12, mF, vb_mask, score);

    // remain only inlier matches
    std::vector<cv::DMatch> _v_matches_12 = v_matches_12;
    v_matches_12.clear();
    for(size_t i = 0; i < _v_matches_12.size(); i++) {
      if(vb_mask[i])  {
        v_matches_12.push_back(_v_matches_12[i]); 
      }
    }

    std::cout << "Score = " << score
              << " / " << v_matches_12.size() <<  std::endl;

    if(false) DrawEpiLines(frame_1st, frame_2nd, v_matches_12, vb_mask, mF);

    // decompose E
    cv::Mat mE = mK.t() * mF * mK;
    cv::Mat T_01 = Solver::DecomposeE(frame_1st.GetKeyPoints(), frame_2nd.GetKeyPoints(), v_matches_12, mK, mE);

    // Triangulation

    // Bundle Adjustment

    return num_map_points;
  }

  System::InitialReconstruction System::FlexibleInitializeGlobalMap(std::vector<std::reference_wrapper<Frame>>& v_frames) {
    InitialReconstruction result;
    int num_map_points = -1; 

    int num_pair_frame = (int)v_frames.size();

    Matcher matcher(ConfigLoader::LoadMatcherConfig(m_config_file));
    std::vector<std::vector<std::vector<cv::DMatch>>> vvv_matches;
    vvv_matches.resize(num_pair_frame);
    for(int i = 0; i < num_pair_frame; ++i) {
      vvv_matches[i].resize(num_pair_frame);
    }

    for(int i = 0; i < (int)vvv_matches.size()-1; ++i) {
      for(int j = i; j < (int)vvv_matches[i].size(); ++j) {
        if(i == j) continue;
        std::vector<cv::DMatch> v_matches_ij = matcher.GetMatches(v_frames[i],v_frames[j]);
        vvv_matches[i][j] = v_matches_ij;
        std::vector<cv::DMatch> v_matches_ji = matcher.Inverse(v_matches_ij);
        vvv_matches[j][i] = v_matches_ji;
      }
    }

    // Compute Fundamental Matrix
    cv::Mat mK = (cv::Mat_<float>(3,3) << m_camera.f_fx, 0.0, m_camera.f_cx,
                                          0.0, m_camera.f_fy, m_camera.f_cy,
                                          0.0,           0.0,           1.0);

    const int center_frame_idx = (int)(v_frames.size() - 1)/2;

    std::vector<KeyFrame> v_keyframes(v_frames.size());
    std::vector<MapPoint> v_mappoints;

    // Initialization using 2 views geometry
    // This is initializatio in initialization.
    {
      const int src_frame_idx = center_frame_idx;
      const int dst_frame_idx = center_frame_idx + 1;
      Frame& src_frame = v_frames[src_frame_idx].get();
      Frame& dst_frame = v_frames[dst_frame_idx].get();

      std::vector<cv::DMatch> v_matches = vvv_matches[src_frame_idx][dst_frame_idx];
      cv::Mat mF;
      std::vector<bool> vb_mask;
      int score;
      Solver::SolveEpipolarConstraintRANSAC(src_frame.GetImage(), dst_frame.GetImage(),  
                                            std::make_pair(src_frame.GetKeyPoints(),dst_frame.GetKeyPoints()),
                                            v_matches, mF, vb_mask, score);

      std::vector<cv::DMatch> _v_matches = v_matches;
      v_matches.clear();
      for(size_t i = 0; i < _v_matches.size(); i++) {
        if(vb_mask[i])  {
          v_matches.push_back(_v_matches[i]); 
        }
      }

      std::cout << "Score = " << score
                << " / " << _v_matches.size() <<  std::endl;

      if(false) DrawEpiLines(src_frame, dst_frame, v_matches, vb_mask, mF);

      // decompose E
      cv::Mat mE = mK.t() * mF * mK;
      cv::Mat T_01 = Solver::DecomposeE(src_frame.GetKeyPoints(), dst_frame.GetKeyPoints(), v_matches, mK, mE);
      src_frame.SetMatchesToNew(v_matches);
      dst_frame.SetMatchesToOld(v_matches);

      // Triangulation
      std::vector<cv::Point3f> v_pts_3d = Solver::Triangulate(src_frame.GetKeyPoints(),
                                                              dst_frame.GetKeyPoints(),
                                                              v_matches, mK, T_01); 

      src_frame.SetPose(cv::Mat::eye(3,4,CV_32FC1));
      dst_frame.SetPose(T_01);

      int _i = 0;
      for(auto pt_3d : v_pts_3d) {
        // std::cout << _i << "L"<< v_pts_3d.size() <<std::endl;
        MapPoint mappoint(pt_3d); 
        cv::Mat desc = ChooseDescriptor(src_frame, dst_frame, pt_3d, v_matches[_i]);
        mappoint.SetDescriptor(desc);
        std::vector<MatchInfo> v_match_info(2);
        v_match_info[0] = MatchInfo{src_frame.m_id, v_matches[_i].queryIdx};
        v_match_info[1] = MatchInfo{dst_frame.m_id, v_matches[_i].trainIdx};
        mappoint.SetMatchInfo(v_match_info);
        if(mappoint.Activate()) {
          v_mappoints.push_back(mappoint);
        }
        _i++;
      }

      v_keyframes[src_frame_idx] = KeyFrame(src_frame);
      v_keyframes[dst_frame_idx] = KeyFrame(dst_frame);

      {
        std::vector<std::reference_wrapper<KeyFrame>> ref_v_keyframes(v_keyframes.begin(), v_keyframes.end());
        std::vector<std::reference_wrapper<MapPoint>> ref_v_mappoints(v_mappoints.begin(), v_mappoints.end());
         BAResult result = BundleAdjustmentBeta(ref_v_keyframes, ref_v_mappoints, m_camera);
      }
    }

    // Do initialization using Map build in 2-view reconstruction.
    {
      bool is_done = false;
      for(int dist_from_center_to_src = 0; !is_done ;dist_from_center_to_src++) {
        const std::vector<int> v_direction = {1,-1}; // forward and backward
        for(int direction : v_direction) {
          int src_frame_idx = direction*dist_from_center_to_src + center_frame_idx;
          int dst_frame_idx = src_frame_idx + direction;
          std::cout << src_frame_idx << ":" << dst_frame_idx << std::endl;
          ///////////////////////////////////////////////////////////////////

          if(CheckIndex(src_frame_idx, dst_frame_idx, v_keyframes, (int)v_frames.size())) {
            is_done = true;
            break;
          } 

          if(v_keyframes[src_frame_idx].IsActivated() && v_keyframes[dst_frame_idx].IsActivated()) {
            // This case was used already.
            continue;
          }
          ///////////////////////////////////////////////////////////////////
          std::vector<std::vector<cv::DMatch>> v_matches_src_to_map;
          for(size_t idx_kf = 0; idx_kf < v_keyframes.size(); idx_kf++) {
            if (v_keyframes[idx_kf].IsActivated()) {
              v_matches_src_to_map.push_back(vvv_matches[src_frame_idx][idx_kf]);
            }
          }

          IncrementalSfM(v_keyframes, v_mappoints, v_frames[src_frame_idx], v_matches_src_to_map, m_initializer_config);

        }
      }
    }

    // Do initialization using Map build in 2-view reconstruction.
    // {
    //   bool is_done = false;
    //   for(int dist_from_center_to_src = 0; !is_done ;dist_from_center_to_src++) {
    //     const std::vector<int> v_direction = {1,-1}; // forward and backward
    //     for(int direction : v_direction) {
    //       int src_frame_idx = direction*dist_from_center_to_src + center_frame_idx;
    //       int dst_frame_idx = src_frame_idx + direction;
    //       std::cout << src_frame_idx << ":" << dst_frame_idx << std::endl;
    //       ///////////////////////////////////////////////////////////////////
    //       if(dst_frame_idx < src_frame_idx) {
    //         std::swap(dst_frame_idx, src_frame_idx);
    //       }
    // 
    //       if(CheckIndex(src_frame_idx, dst_frame_idx, vb_initialized, (int)v_frames.size())) {
    //         is_done = true;
    //         break;
    //       } 
    // 
    //       if(vb_initialized[src_frame_idx] && vb_initialized[dst_frame_idx]) {
    //         // This case was used already.
    //         continue;
    //       }
    //       ///////////////////////////////////////////////////////////////////
    // 
    //       cv::Mat base_pose;
    //       if(direction == 1) {
    //         base_pose = AppendRow(v_keyframes[src_frame_idx].GetPose());
    //         // std::cout << v_keyframes[src_frame_idx].GetPose() << std::endl;
    //       }
    //       else {
    //         base_pose = AppendRow(Inverse3x4(v_keyframes[dst_frame_idx].GetPose()));
    //         // std::cout << v_keyframes[dst_frame_idx].GetPose() << std::endl;
    //       }
    // 
    //       // std::cout << base_pose << std::endl;
    //       // std::cout << "==================" << std::endl;
    // 
    //       Frame& src_frame = v_frames[src_frame_idx].get();
    //       Frame& dst_frame = v_frames[dst_frame_idx].get();
    // 
    //       std::vector<cv::DMatch> v_matches = vv_matches[src_frame_idx];
    //       cv::Mat mF;
    //       std::vector<bool> vb_mask;
    //       int score;
    //       Solver::SolveEpipolarConstraintRANSAC(src_frame.GetImage(), dst_frame.GetImage(),  
    //                                             std::make_pair(src_frame.GetKeyPoints(),dst_frame.GetKeyPoints()),
    //                                             v_matches, mF, vb_mask, score);
    // 
    //       std::vector<cv::DMatch> _v_matches = v_matches;
    //       v_matches.clear();
    //       for(size_t i = 0; i < _v_matches.size(); i++) {
    //         if(vb_mask[i])  {
    //           v_matches.push_back(_v_matches[i]); 
    //         }
    //       }
    // 
    //       std::cout << "Score = " << score
    //                 << " / " << _v_matches.size() <<  std::endl;
    // 
    //       if(false) DrawEpiLines(src_frame, dst_frame, v_matches, vb_mask, mF);
    // 
    //       // decompose E
    //       cv::Mat mE = mK.t() * mF * mK;
    //       cv::Mat T_01 = Solver::DecomposeE(src_frame.GetKeyPoints(), dst_frame.GetKeyPoints(), v_matches, mK, mE);
    //       src_frame.SetMatchesToNew(v_matches);
    //       dst_frame.SetMatchesToOld(v_matches);
    // 
    //       // Triangulation
    //       std::vector<cv::Point3f> v_pts_3d = Solver::Triangulate(src_frame.GetKeyPoints(),
    //                                                               dst_frame.GetKeyPoints(),
    //                                                               v_matches, mK, T_01); 
    // 
    //       if(direction == 1) {
    //         // src->dst(uninitialized)
    //         if(vb_initialized[src_frame_idx]) {
    //           // src_frame.SetPose(cv::Mat::eye(3,4,CV_32FC1));
    //           dst_frame.SetPose(T_01 * base_pose);
    //         }
    //         else {
    //           std::cout << "[WARNING::System] Initialization index has a bug" << std::endl;
    //         }
    //       }
    //       else {
    //         // src(uninitialized)->dst->base
    //         if(vb_initialized[dst_frame_idx]) {
    //           // dst_frame.SetPose(cv::Mat::eye(3,4,CV_32FC1));
    //           src_frame.SetPose(Inverse3x4(T_01) * base_pose);
    //         }
    //         else {
    //           std::cout << "[WARNING::System] Initialization index has a bug" << std::endl;
    //         }
    //       }
    // 
    //       int _i = 0;
    //       for(cv::Point3f _pt_3d : v_pts_3d) {
    //         // src-coordinate
    //         cv::Mat pt_3d;
    //         if(direction == 1) {
    //           const float x = _pt_3d.x;
    //           const float y = _pt_3d.y;
    //           const float z = _pt_3d.z;
    //           cv::Mat _pt_3d = (cv::Mat_<float>(4,1) << x,
    //                                                     y,
    //                                                     z,
    //                                                     1.0);
    //           pt_3d = base_pose * _pt_3d;
    //         }
    //         else if(direction == -1){
    //           const float x = _pt_3d.x;
    //           const float y = _pt_3d.y;
    //           const float z = _pt_3d.z;
    //           cv::Mat _pt_3d = (cv::Mat_<float>(4,1) << x,
    //                                                     y,
    //                                                     z,
    //                                                     1.0);
    //           pt_3d = base_pose * _pt_3d;
    //         }
    //         else {
    //           std::cout << "[WARNING] Critical !!!!!!!!!!" << std::endl;
    //         }
    //         MapPoint mappoint(pt_3d.at<float>(0),pt_3d.at<float>(1),pt_3d.at<float>(2)); 
    //         cv::Mat desc = ChooseDescriptor(src_frame, dst_frame, mappoint.GetPosition(), v_matches[_i]);
    //         mappoint.SetDescriptor(desc);
    //         std::vector<MatchInfo> v_match_info(2);
    //         v_match_info[0] = MatchInfo{src_frame.m_id, v_matches[_i].queryIdx};
    //         v_match_info[1] = MatchInfo{dst_frame.m_id, v_matches[_i].trainIdx};
    //         mappoint.SetMatchInfo(v_match_info);
    //         if(mappoint.Activate()) {
    //           v_mappoints.push_back(mappoint);
    //         }
    //         ++_i;
    //       }
    // 
    //       if(!vb_initialized[dst_frame_idx])
    //         v_keyframes[dst_frame_idx] = KeyFrame(dst_frame);
    //       if(!vb_initialized[src_frame_idx])
    //         v_keyframes[src_frame_idx] = KeyFrame(src_frame);
    // 
    //       if(direction == 1)
    //         vb_initialized[dst_frame_idx] = true;
    //       else
    //         vb_initialized[src_frame_idx] = true;
    // 
    //       // Optimization would be processed here.
    //       {
    //          BAResult result = BundleAdjustmentBeta(v_keyframes, v_mappoints, m_camera);
    //       }
    //     }
    //   }
    // }

    // Convert frames to keyframes
    // All data should be inserted to Map

#if 0
    {
      using namespace open3d;
      utility::SetVerbosityLevel(utility::VerbosityLevel::Debug);
      auto cloud_ptr = std::make_shared<geometry::PointCloud>();
      std::vector<Eigen::Vector3d> vd_points;
      for(int i = 0; i < 30; ++i) {
        for(int j = 0; j < 30; ++j) {
          Eigen::Vector3d vec3{(double)i, (double)j, 0.0};
          vd_points.push_back(vec3);
        }
      }
      cloud_ptr->points_ = vd_points;
      cloud_ptr->NormalizeNormals();
      visualization::DrawGeometries({cloud_ptr}, "PointCloud", 1600, 900);
      utility::LogInfo("End of the test.\n");
    }
#endif

    return result;
  }

  int System::IncrementalSfM(std::vector<KeyFrame>& v_keyframes, 
                     std::vector<MapPoint>& v_mappoints, 
                     Frame& f,
                     const std::vector<std::vector<cv::DMatch>>& v_matches,
                     const InitializerConfig _config) {

    //1. Get matches between map and input frame using matches of keyframes
    for(size_t kf_idx = 0; kf_idx < v_keyframes.size(); kf_idx++) {
      if(std::abs((int)kf_idx - f.m_id) == 0 && std::abs((int)kf_idx - f.m_id) > _config.connect_distance) {
        continue;
      }
    }

    //2. SolvePnP

    //3. Perform BundleAdjustment

    //Finally, input frame is registered as keyframe and mappoints are inserted to map

    return 0;
  }

  void System::Run() {
    std::cout << "[LOG] " 
              << "Start Processing ..."
              << std::endl;


    InitializeFrames(m_v_frames, m_initializer_config.num_frames);

    std::vector<Frame> v_ini_frames; v_ini_frames.reserve(m_initializer_config.num_frames);
    for(int i = 0; i < m_initializer_config.num_frames; ++i) {
      v_ini_frames.push_back(m_v_frames[i]);
    }
#if 0
    InitializeGlobalMap(v_ini_frames);
#else
    std::vector<std::reference_wrapper<Frame>> ref_v_ini_frames(v_ini_frames.begin(), v_ini_frames.end());
    InitialReconstruction result = FlexibleInitializeGlobalMap(ref_v_ini_frames);
    // Map is initialized here.
    m_p_map->Initialize(result.v_keyframes, result.v_mappoints);

#endif

    std::cout << "=============================" << std::endl;
    if (m_p_viewer->Run() != 0) {
      std::cout << "[LOG] Viewer is broken" << std::endl;
    }

    std::cout << "=============================" << std::endl;
    std::cout << std::endl;
    return;
  };

  void System::ShowConfig() {
    std::cout << "[Config.path2images] "
              << m_config.str_path_to_images
              << std::endl;

    std::cout << "[Images] " 
              << m_vm_images.size() 
              << std::endl;
    
    return;
  }

} // namespace
