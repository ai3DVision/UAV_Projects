#include "main_controller.h"

#include <iostream>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <chrono>

#include <QPixmap>
#include <QPointF>
#include <QDebug>

#include <opencv2/features2d.hpp>
#include <opencv2/calib3d.hpp>

#include "utils/acmopencv.h"
#include "utils/csv.h"
#include "utils/gradient_density.h"
#include "utils/geom_utils.h"

#include "config_singleton.h"
#include "algorithms/trajectory_loader.h"
#include "algorithms/transformator.h"

using namespace std;
using namespace modelpkg;
using namespace controllerpkg;
using namespace algorithmspkg;

MainController::MainController()
{
    initDetectors();
    initDescriptors();
    trj_recovers.assign(2, TrajectoryRecover(detectors[0],
                                             descriptors[0]));

    trajectories_selected_frames.assign(2, vector<int>());
    accumulative_trj_cuts.assign(2, vector<int>());
}

void MainController::loadOrCalculateModel(int detector_idx, int descriptor_idx)
{
    loadOrCalculateKeyPoints(detector_idx);
    loadOrCalculateDescriptions(detector_idx, descriptor_idx);

    for (size_t trj_num = 0; trj_num < model->getTrajectoriesCount(); trj_num++)
    {
        auto &trj = model->getTrajectory(trj_num);
        TrajectoryRecover &recover = trj_recovers[trj_num];
        recover.setDetector(detectors[detector_idx]);
        recover.setDescriptor(descriptors[descriptor_idx]);

        recover.clear();
        for (size_t frame_num = 0; frame_num < trj.getFramesCount(); frame_num++)
        {
            recover.addFrame(trj.getFrame(frame_num).image,
                             trj.getFrameAllKeyPoints(frame_num),
                             trj.getFrameDescription(frame_num),
                             trj.getFrame(frame_num).pos_m,
                             -trj.getFrame(frame_num).angle,
                             trj.getFrame(frame_num).m_per_px);
        }
    }

    for (size_t trj_num = 0; trj_num < model->getTrajectoriesCount(); trj_num++)
    {
        this->showKeyPoints(trj_num);
    }
}

void MainController::calculateMatches(int descriptor_idx)
{
    //norm_type for each descriptor setted
    //cv::BFMatcher matcher(norm_types[descriptor_idx]);
    cv::FlannBasedMatcher matcher;

    vector<vector<cv::KeyPoint>> trajectories_kp_clouds(2, vector<cv::KeyPoint>());
    vector<cv::Mat> trajectories_descr_clouds(2, cv::Mat());

    //generate descriptors and key_points clouds
    for (size_t trj_num = 0; trj_num < model->getTrajectoriesCount(); trj_num++)
    {
        const auto &trj = model->getTrajectory(trj_num);
        const auto &selected_frames = trajectories_selected_frames[trj_num];
        vector<cv::Mat> descriptions_cloud;
        auto &key_points_cloud = trajectories_kp_clouds[trj_num];
        auto &accumulative_cut = accumulative_trj_cuts[trj_num];

        //accumulative_cut need for further relevant key points extraction
        accumulative_cut.clear();
        accumulative_cut.push_back(0);
        if (selected_frames.empty())
        {//take all trajectory
            for (const auto &frame_key_points: trj.getAllKeyPoints())
            {
                key_points_cloud.insert(key_points_cloud.end(),
                                        frame_key_points.begin(), frame_key_points.end());

                //sum of all counts of key_poins on frame
                accumulative_cut.push_back(accumulative_cut.back() + frame_key_points.size());
            }
            for (const auto &description: trj.getAllDescriptions())
            {
                if (!description.empty())
                {
                    descriptions_cloud.push_back(description);
                }
            }
        }
        else
        {//only selected
            for (const auto &frame_num: selected_frames)
            {
                const auto &frame_key_points = trj.getFrameAllKeyPoints(frame_num);
                key_points_cloud.insert(key_points_cloud.end(),
                                        frame_key_points.begin(), frame_key_points.end());
                descriptions_cloud.push_back(trj.getFrameDescription(frame_num));

                //sum of all counts of key_poins on frame
                accumulative_cut.push_back(accumulative_cut.back() + frame_key_points.size());
            }
        }

        cv::vconcat(descriptions_cloud, trajectories_descr_clouds[trj_num]);

        //it need for flann matcher | http://stackoverflow.com/a/11798593/2627487
        if(trajectories_descr_clouds[trj_num].type() != CV_32F) {
            trajectories_descr_clouds[trj_num].convertTo(trajectories_descr_clouds[trj_num], CV_32F);
        }
    }

    isFirstMatchingOnSecond = trajectories_selected_frames[1].empty();
    int from_trj_num = isFirstMatchingOnSecond? 0: 1;
    int to_trj_num = isFirstMatchingOnSecond? 1: 0;
    vector<cv::DMatch> rough_matches;

    matcher.add(trajectories_descr_clouds[to_trj_num]);
    matcher.train();

    auto start_match_time = chrono::high_resolution_clock::now();

    matcher.match(trajectories_descr_clouds[from_trj_num],
                  rough_matches);

    auto finish_match_time = chrono::high_resolution_clock::now();
    clog << "Match time: " <<
            chrono::duration_cast<chrono::milliseconds>(finish_match_time - start_match_time).count() <<
            "ms" << endl;

    //prepare points for findHomography | using trajectories_kp_cloud
    //we need to transform to_trj_pts to points on map
    vector<cv::Point2f> from_trj_pts;
    vector<cv::Point2f> to_trj_pts;

    //for (int i = 0; i < rough_matches.size(); i++)
    for (const cv::DMatch &match: rough_matches)
    {
        //here I assume that choosed one frame otherwise shitty code like for to_trj_pts
        from_trj_pts.push_back(trajectories_kp_clouds[from_trj_num][match.queryIdx].pt);
        //we need to transform to_trj_pts from points on image to points on map
        //SHITTY CODE START (doubled from showMatches() )
        int frame_num = 0;

        //to_trj - it's always train
        const auto &idx = match.trainIdx;

        //calculating frame_num and kp_num
        while (idx >= accumulative_trj_cuts[to_trj_num][frame_num])
        {
            frame_num++;
        }
        frame_num--; //because start from 0

        int kp_num = idx - accumulative_trj_cuts[to_trj_num][frame_num];

        const Map &frame = model->getTrajectory(to_trj_num).getFrame(frame_num);
        const cv::KeyPoint &kp = model->getTrajectory(to_trj_num).getFrameKeyPoint(frame_num, kp_num);
        QPointF kp_translated_to_center(kp.pt.x - frame.image.cols/2., kp.pt.y - frame.image.rows/2.);
        QPointF rotated_kp = QTransform().rotate(frame.angle).map(kp_translated_to_center);

        QPointF pt_on_map( rotated_kp*frame.m_per_px + utils::cv::toQPointF(frame.pos_m) /* frame.m_per_px*/);

        to_trj_pts.push_back(cv::Point2f(pt_on_map.x(), pt_on_map.y()));
        //to_trj_pts.push_back(trajectories_kp_cloud[to_trj_num][match.trainIdx].pt);
    }

    auto start_homo_time = chrono::high_resolution_clock::now();

    vector<char> mask;   
    cv::Mat homography = cv::findHomography(from_trj_pts, to_trj_pts, cv::RANSAC, 3, mask);

    auto finish_homo_time = chrono::high_resolution_clock::now();
    clog << "Finding homography time: " <<
            chrono::duration_cast<chrono::milliseconds>(finish_homo_time - start_homo_time).count() <<
            "ms" << endl;


    //setting ghost recover
    const Map &frame = model->getTrajectory(from_trj_num).getFrame(trajectories_selected_frames[from_trj_num].front());
    /*cv::Point2f cv_center(frame.image.cols/2., frame.image.rows/2.);
    vector<cv::Point2f> tmp_in(1, cv_center);
    vector<cv::Point2f> tmp_out;
    cv::perspectiveTransform(tmp_in, tmp_out, homography);
    cv::Point2f cv_center_transformed = tmp_out[0];*/

    QSize size(frame.image.cols, frame.image.rows);
    cv::Point2f center(frame.image.cols/2., frame.image.rows/2.);
    cv::Point2f rotate_pt(center.x+10, center.y);

    cv::Point2f bounded_center = algorithmspkg::Transformator::transform(center, homography);
    cv::Point2f bounded_rotate_pt = algorithmspkg::Transformator::transform(rotate_pt, homography);


    QPointF center_px(bounded_center.x, bounded_center.y);
    double angle = utils::cv::angleBetween(rotate_pt-center, bounded_rotate_pt - bounded_center);
    //double angle = atan(homography.at<double>(1, 0) / homography.at<double>(0, 0))*180/M_PI;
    double coords_m_per_px = 1;//model->getTrajectory(to_trj_num).getFrame(0).m_per_px;
    double m_per_px = cv::norm(bounded_rotate_pt - bounded_center) / cv::norm(rotate_pt - center);
    cout << "length to " << cv::norm(bounded_rotate_pt - bounded_center) << endl;
    cout << "length from " << cv::norm(rotate_pt - center) << endl;
    cout << "scale " << m_per_px << endl;
    //double m_per_px = frame.m_per_px;

    view->setGhostRecovery(center_px, size, angle,
                           m_per_px, coords_m_per_px);

    //setting matches
    matches.clear();
    int mask_true_count = 0;
    for (int i = 0; i < mask.size(); i++)
    {
        if (mask[i])
        {
            matches.push_back(rough_matches[i]);
            mask_true_count++;
        }
    }
    cout << "Score: " << mask_true_count / double(mask.size()) << endl;
    //matches.assign(rough_matches.begin(), rough_matches.end());

    this->showMatches();
}

void MainController::recoverTrajectory(double score_thres)
{
    TrajectoryRecover &recover = this->trj_recovers[0];
    Trajectory &trj = model->getTrajectory(1);

    frames_to_hide_by_score.clear();
    for (int frame_num = 0; frame_num < trj.getFramesCount(); frame_num++)
    {
        Map &frame = trj.getFrame(frame_num);

        cv::Mat homography;
        std::vector<cv::DMatch> matches;
        double score = recover.recoverTrajectory(trj.getFrameAllKeyPoints(frame_num),
                                  trj.getFrameDescription(frame_num),
                                  homography, matches);
        cout << "Score: " << score << endl;

        if (score > score_thres)
        {
          cv::Point2f center(frame.image.cols/2., frame.image.rows/2.);
          cv::Point2f rotate_pt(center.x+10, center.y);

          cv::Point2f bounded_center = Transformator::transform(center, homography);
          cv::Point2f bounded_rotate_pt = Transformator::transform(rotate_pt, homography);

          frame.pos_m = bounded_center;
          frame.angle = utils::cv::angleBetween(rotate_pt-center,
                                                bounded_rotate_pt - bounded_center);
          frame.m_per_px = cv::norm(bounded_rotate_pt - bounded_center) / cv::norm(rotate_pt - center);
        }
        else
        {
          frames_to_hide_by_score.push_back(frame_num);
        }
    }


    showTrajectory(1);
}

void MainController::loadIni(string ini_filename)
{
    clear();

    ConfigSingleton &cfg = ConfigSingleton::getInstance();
    try
    {
        cfg.loadIni(ini_filename);
        loadTrajectories(cfg.getPathToTrajectoryCsv(0),
                         cfg.getPathToTrajectoryCsv(1));

        loadMainMap(cfg.getPathToMapCsv(),
                    cfg.getMapMetersPerPixel());

        view->setTrajectoryPath(0, QString::fromStdString(
                                  cfg.getPathToTrajectoryCsv(0)));
        view->setTrajectoryPath(1, QString::fromStdString(
                                  cfg.getPathToTrajectoryCsv(1)));
    }
    catch (ConfigSingleton::Exception &e)
    {
        this->showException(e.what());
    }
}

void MainController::loadTrajectories(string trj1_filename, string trj2_filename)
{
    model->setTrajectory(0, loadTrjFromCsv(trj1_filename));
    model->setTrajectory(1, loadTrjFromCsv(trj2_filename));

    this->calculateFramesQuality();

    for (size_t trj_num = 0; trj_num < model->getTrajectoriesCount(); trj_num++)
    {
        this->showTrajectory(trj_num);
    }
}

void MainController::loadTrajectory(int trj_num, string trj_filename)
{
  ConfigSingleton &cfg = ConfigSingleton::getInstance();
  try
  {
    Trajectory new_trj = loadTrjFromCsv(trj_filename);

    model->setTrajectory(trj_num, new_trj);
    this->calculateFramesQuality(trj_num);

    cfg.setPathToTrajectoryCsv(trj_num, trj_filename);

    this->showTrajectory(trj_num);
  }
  catch (runtime_error er)
  {
      showException(er.what());

      QString prev = QString::fromStdString(cfg.getPathToTrajectoryCsv(trj_num));
      view->setTrajectoryPath(trj_num, prev);
  }
}

void MainController::loadMainMap(string filename, double meters_per_pixel)
{
    model->getMainMap() = modelpkg::Map(filename, 0, 0, 0, meters_per_pixel);

    double center_x_m = model->getMainMap().image.cols / 2.0 * meters_per_pixel;
    double center_y_m = model->getMainMap().image.rows / 2.0 * meters_per_pixel;
    model->getMainMap().pos_m = cv::Point2f(center_x_m, center_y_m);

    this->showMainMap();
}

void MainController::showTrajectory(int trj_num)
{
    vector<QPixmap> qpixs;
    vector<QPointF> center_coords_px;
    vector<double>  angles;
    vector<double>  meter_per_pixels;
    const vector<double> &qualities = model->getTrajectory(trj_num).getAllFramesQuality();

    for (const auto &frame: model->getTrajectory(trj_num).getAllFrames())
    {
        QPointF center_px;
        center_px.setX( frame.pos_m.x / frame.m_per_px );
        center_px.setY( frame.pos_m.y / frame.m_per_px );

        qpixs.push_back( utils::ASM::cvMatToQPixmap(frame.image) );
        center_coords_px.push_back( center_px );
        angles.push_back( frame.angle );
        meter_per_pixels.push_back( frame.m_per_px );
    }
    if (trj_num == 0)
    {
        view->setFirstTrajectory(qpixs, center_coords_px, angles, meter_per_pixels, qualities);
    }
    else
    {
        view->setSecondTrajectory(qpixs, center_coords_px, angles, meter_per_pixels, qualities);
    }
}

void MainController::showMainMap()
{
    QPixmap qpix = utils::ASM::cvMatToQPixmap(model->getMainMap().image);

    view->setMainMap(qpix, model->getMainMap().m_per_px);
}

void MainController::showKeyPoints(int trj_num)
{
    const Trajectory &trj = model->getTrajectory(trj_num);
    const auto &key_points = trj.getAllKeyPoints();

    vector<int> frames_num;
    vector<QPointF> center_coords_px;
    vector<double> angles;
    vector<double> radius;
    vector<QColor> colors;

    for (size_t frame_num = 0; frame_num < trj.getFramesCount(); frame_num++)
    {
        for (size_t kp_num = 0; kp_num < std::min((size_t)100, key_points[frame_num].size()); kp_num++)
        {
            const Map &frame = trj.getFrame(frame_num);
            const cv::KeyPoint &kp = key_points[frame_num][kp_num];

            double mul = kp.response / 100;
            if (mul > 1) mul = 1;

            frames_num.push_back(frame_num);
            //pos_m - it's center of frame, so we need postpone vector (pt - image.center) from frame.centr
            center_coords_px.push_back( utils::cv::toQPointF(kp.pt) - QPointF(frame.image.cols, frame.image.rows)/2 +
                                        utils::cv::toQPointF(frame.pos_m) / frame.m_per_px);
            angles.push_back(kp.angle);
            radius.push_back(kp.size / 2.);
            colors.push_back( QColor(255*(1-mul), 255*(mul), 0) );//QColor(255*mul, 165*mul, 0));
        }
    }

    if (trj_num == 0)
    {
        view->setFirstKeyPoints(frames_num, center_coords_px, angles, radius, colors);
    }
    else
    {
        view->setSecondKeyPoints(frames_num, center_coords_px, angles, radius, colors);
    }
}

void MainController::showKeyPointsNew(int trj_num)
{
    const Trajectory &trj = model->getTrajectory(trj_num);
    TrajectoryRecover &recover = trj_recovers[trj_num];
    const auto &kps_cloud = recover.getKeyPointsCloud();

    int i = 0;
    for (const cv::KeyPoint &kp: kps_cloud)
    {
      if (kp.response > 50)
      {
        view->getTrajectoryItem(trj_num).addKeyPointNew(
              utils::cv::toQPointF(kp.pt),
              kp.angle,
              kp.size/2.,
              1,
              QColor(255, 0, 0));
      }
      i++;
    }

}

void MainController::showMatches()
{
    //DMatch query_idx = first_trj, train_idx = second_trj
    vector<vector<QPointF>> trajectories_kp(matches.size(), vector<QPointF>(model->getTrajectoriesCount(), QPointF(0, 0)));
    vector<vector<QPointF>> frames_center_on_map(matches.size(), vector<QPointF>());
    vector<vector<double>> angles(matches.size(), vector<double>());
    vector<vector<double>> meters_per_pixels(matches.size(), vector<double>());

    for (size_t match_num = 0; match_num < matches.size(); match_num++)
    {
        const cv::DMatch &match = matches[match_num];
        vector<int> frame_nums(model->getTrajectoriesCount(), 0);
        vector<int> kp_nums(model->getTrajectoriesCount(), 0);

        for (size_t trj_num = 0; trj_num < model->getTrajectoriesCount(); trj_num++)
        {
            const auto &trj = model->getTrajectory(trj_num);
            const auto &accumulate_cuts = accumulative_trj_cuts[trj_num];
            const auto &selected_frames = trajectories_selected_frames[trj_num];
            auto &frame_num = frame_nums[trj_num];
            auto &kp_num = kp_nums[trj_num];

            //tricky code
            const auto &idx = isFirstMatchingOnSecond == trj_num? match.trainIdx:  match.queryIdx;

            //calculating frame_num and kp_num
            while (idx >= accumulate_cuts[frame_num])
            {
                frame_num++;
            }
            frame_num--; //because start from 0

            kp_num = idx - accumulate_cuts[frame_num];

            if (!selected_frames.empty())
            {
                frame_num = selected_frames[frame_num];
            }


            //get point
            const Map &frame = trj.getFrame(frame_num);
            const cv::KeyPoint &kp = trj.getFrameKeyPoint(frame_num, kp_num);
            trajectories_kp[match_num][trj_num] = utils::cv::toQPointF(kp.pt) - QPointF(frame.image.cols, frame.image.rows)/2 +
                                               utils::cv::toQPointF(frame.pos_m) / frame.m_per_px;
            frames_center_on_map[match_num].push_back(utils::cv::toQPointF(frame.pos_m) / frame.m_per_px);
            angles[match_num].push_back(frame.angle);
            meters_per_pixels[match_num].push_back(frame.m_per_px);
        }
    }
    view->setMatches(trajectories_kp, frames_center_on_map, angles, meters_per_pixels);
}

void MainController::filterByScore(bool isFilter)
{
    if (isFilter)
    {
      for (int frame_num: frames_to_hide_by_score)
      {
        view->getTrajectoryItem(1).hideFrame(frame_num);
      }
    }
    else
    {
      for (int frame_num: frames_to_hide_by_score)
      {
        view->getTrajectoryItem(1).showFrame(frame_num);
      }
    }
}

void MainController::selectedFrame(int trj_num, int frame_num)
{
    trajectories_selected_frames[trj_num].push_back(frame_num);
}

void MainController::unselectedFrame(int trj_num, int frame_num)
{
    auto &selected_frames = trajectories_selected_frames[trj_num];
    auto where = find(selected_frames.begin(), selected_frames.end(), frame_num);
    if (where != selected_frames.end())
    {
        selected_frames.erase(where);
    }
}

void MainController::showException(string what)
{
    clog << "Exception! " << what << endl;
    view->showException(QString::fromStdString(what));
}

void MainController::showView()
{
    view->setDetectors(detectors_names);
    view->setDescriptors(descriptors_names);
    view->show();
}

/*
 * PRIVATE SECTION
 */

void MainController::calculateFramesQuality()
{
    double threshold = ConfigSingleton::getInstance().getQualityThreshold();

    for (size_t trj_num = 0; trj_num < model->getTrajectoriesCount(); trj_num++)
    {
        Trajectory &trj = model->getTrajectory(trj_num);

        for (size_t frame_num = 0; frame_num < trj.getFramesCount(); frame_num++)
        {
            const auto &frame = trj.getFrame(frame_num);

            double scale = frame.m_per_px / ConfigSingleton::getInstance().getGradientMetersPerPixel();
            cv::Size new_size(frame.image.size().width * scale, frame.image.size().height * scale);

            cv::Mat resized;
            cv::resize(frame.image, resized, new_size);

            double quality = std::min( 1., utils::cv::gradientDensity(resized) / threshold );

            trj.setFrameQuality(frame_num, quality);
        }
    }

}

void MainController::calculateFramesQuality(int trj_num)
{
  double threshold = ConfigSingleton::getInstance().getQualityThreshold();
  Trajectory &trj = model->getTrajectory(trj_num);

  for (size_t frame_num = 0; frame_num < trj.getFramesCount(); frame_num++)
  {
      const auto &frame = trj.getFrame(frame_num);

      double scale = frame.m_per_px / ConfigSingleton::getInstance().getGradientMetersPerPixel();
      cv::Size new_size(frame.image.size().width * scale, frame.image.size().height * scale);

      cv::Mat resized;
      cv::resize(frame.image, resized, new_size);

      double quality = std::min( 1., utils::cv::gradientDensity(resized) / threshold );

      trj.setFrameQuality(frame_num, quality);
  }
}

/*
 * Key points
 */
void MainController::loadOrCalculateKeyPoints(int detector_idx)
{
    ConfigSingleton &cfg = ConfigSingleton::getInstance();

    for (size_t trj_num = 0; trj_num < model->getTrajectoriesCount(); trj_num++)
    {
        string path_to_kp_bin = cfg.getPathToKeyPoints(trj_num, detectors_names[detector_idx].toStdString());

        try
        {
            loadKeyPoints(trj_num, path_to_kp_bin);
        }
        catch (MainController::NoFileExist &e)
        {
            clog << e.what() << endl;
            clog << "Calculating key points for trajectory #" << trj_num+1 << endl;
            calculateKeyPoints(trj_num, detector_idx);

            clog << "Saving key points" << endl;
            saveKeyPoints(trj_num, path_to_kp_bin);
        }
    }
}

void MainController::calculateKeyPoints(int trj_num, int detector_idx)
{
    cv::Ptr<cv::Feature2D> &detector = detectors[detector_idx];
    Trajectory &trj = model->getTrajectory(trj_num);

    for (size_t frame_num = 0; frame_num < trj.getFramesCount(); frame_num++)
    {
        const auto &frame = trj.getFrame(frame_num);
        auto &mutable_frame_kps = trj.getFrameAllKeyPoints(frame_num);

        detector->detect(frame.image, mutable_frame_kps);

        //sort by response
        std::sort(mutable_frame_kps.begin(), mutable_frame_kps.end(), []( const cv::KeyPoint &left, const cv::KeyPoint &right ) ->
                                         bool { return left.response > right.response; });
    }
}

void MainController::loadKeyPoints(int trj_num, string filename)
{
    ifstream in(filename, ios::binary);
    if (!in)
    {
        throw MainController::NoFileExist(filename);
    }

    auto &trj = model->getTrajectory(trj_num);

    //maybe format checking
    size_t framesCount = 0;
    in.read(reinterpret_cast<char*>(&framesCount), sizeof(framesCount));
    for (size_t frame_num = 0; frame_num < framesCount; frame_num++)
    {
        auto &mutable_frame_kps = trj.getFrameAllKeyPoints(frame_num);
        mutable_frame_kps.clear();

        size_t keyPointsCount = 0;
        in.read(reinterpret_cast<char*>(&keyPointsCount), sizeof(keyPointsCount));
        for (size_t kp_num = 0; kp_num < keyPointsCount; kp_num++)
        {
            cv::KeyPoint kp;
            in.read(reinterpret_cast<char*>(&kp), sizeof(kp));
            mutable_frame_kps.push_back(kp);
        }
    }
}

void MainController::saveKeyPoints(int trj_num, string filename)
{
    const auto &trj = model->getTrajectory(trj_num);

    ofstream out(filename, ios::binary);

    size_t framesCount = trj.getFramesCount();
    out.write(reinterpret_cast<char*>(&framesCount), sizeof(framesCount));
    for (size_t frame_num = 0; frame_num < framesCount; frame_num++)
    {
        const auto &frame_key_points = trj.getFrameAllKeyPoints(frame_num);

        size_t keyPointsCount = frame_key_points.size();
        out.write(reinterpret_cast<char*>(&keyPointsCount), sizeof(keyPointsCount));
        for (size_t kp_num = 0; kp_num < keyPointsCount; kp_num++)
        {
            out.write(reinterpret_cast<const char*>(&frame_key_points[kp_num]), sizeof(frame_key_points[kp_num]));
        }
    }
}

/*
 * Descriptions
 */

void MainController::loadOrCalculateDescriptions(int detector_idx, int descriptor_idx)
{
    ConfigSingleton &cfg = ConfigSingleton::getInstance();

    for (size_t trj_num = 0; trj_num < model->getTrajectoriesCount(); trj_num++)
    {
        string path_to_dscr_xml = cfg.getPathToDescriptors(trj_num, detectors_names[detector_idx].toStdString(),
                                                                    descriptors_names[descriptor_idx].toStdString());

        try
        {
            loadDescriptions(trj_num, path_to_dscr_xml);
        }
        catch (MainController::NoFileExist &e)
        {
            clog << e.what() << endl;
            clog << "Calculating descriptors for trajectory #" << trj_num+1 << endl;
            calculateDescriptions(trj_num, detector_idx);

            clog << "Saving descriptors" << endl;
            saveDescriptions(trj_num, path_to_dscr_xml);
        }
    }

}

void MainController::calculateDescriptions(int trj_num, int descriptor_idx)
{
    cv::Ptr<cv::Feature2D> &descriptor = descriptors[descriptor_idx];
    Trajectory &trj = model->getTrajectory(trj_num);

    for (size_t frame_num = 0; frame_num < trj.getFramesCount(); frame_num++)
    {
        cv::Mat descr;
        descriptor->compute(trj.getFrame(frame_num).image, trj.getFrameAllKeyPoints(frame_num), descr);

        trj.setFrameDescription(frame_num, descr);
    }
}

void MainController::loadDescriptions(int trj_num, string filename)
{
    auto &trj = model->getTrajectory(trj_num);

    ifstream in(filename, ios::binary);
    if (!in)
    {
        throw MainController::NoFileExist(filename);
    }

    //maybe format checking
    size_t framesCount = 0;
    in.read(reinterpret_cast<char*>(&framesCount), sizeof(framesCount));
    for (size_t frame_num = 0; frame_num < framesCount; frame_num++)
    {
        int rows = 0;
        in.read(reinterpret_cast<char*>(&rows),
                  sizeof(rows));
        int cols = 0;
        in.read(reinterpret_cast<char*>(&cols),
                  sizeof(cols));

        cv::Mat description(rows, cols, CV_32F);

        in.read(reinterpret_cast<char*>(description.data), rows*cols*sizeof(float));

        trj.setFrameDescription(frame_num, description);
    }
    /*cv::FileStorage file(filename, cv::FileStorage::READ);

    if (!file.isOpened())
    {
        throw MainController::NoFileExist(filename);
    }

    int framesCount = 0;
    file["count"] >> framesCount;
    for (int frame_num = 0; frame_num < framesCount; frame_num++)
    {
        cv::Mat descr;
        file["img"+to_string(frame_num)] >> descr;

        trj.setFrameDescription(frame_num, descr);
    }
    file.release();*/
}

void MainController::saveDescriptions(int trj_num, string filename)
{
    const auto &trj = model->getTrajectory(trj_num);

    ofstream out(filename, ios::binary);

    size_t framesCount = trj.getFramesCount();
    out.write(reinterpret_cast<char*>(&framesCount), sizeof(framesCount));
    for (size_t frame_num = 0; frame_num < framesCount; frame_num++)
    {
        const auto &frame_description = trj.getFrameDescription(frame_num);

        int rows = frame_description.rows;
        out.write(reinterpret_cast<char*>(&rows),
                  sizeof(frame_description.rows));
        int cols = frame_description.cols;
        out.write(reinterpret_cast<char*>(&cols),
                  sizeof(frame_description.cols));

        if (frame_description.type() == CV_32F)
        {
          out.write(reinterpret_cast<char*>(frame_description.data), rows*cols*sizeof(float));
        }
    }

    /*cv::FileStorage file(filename, cv::FileStorage::WRITE);

    int framesCount = trj.getFramesCount();
    file << "count" << framesCount;
    for (int frame_num = 0; frame_num < framesCount; frame_num++)
    {
        file << "img"+to_string(frame_num) << trj.getFrameDescription(frame_num);
    }
    file.release();*/
}

void MainController::initDetectors()
{
    detectors_names.push_back("SIFT");
    detectors.push_back(cv::xfeatures2d::SIFT::create());

    detectors_names.push_back("SURF");
    detectors.push_back(cv::xfeatures2d::SURF::create());

    detectors_names.push_back("KAZE");
    detectors.push_back(cv::KAZE::create());

    detectors_names.push_back("AKAZE");
    detectors.push_back(cv::AKAZE::create());

    detectors_names.push_back("BRISK");
    detectors.push_back(cv::BRISK::create());

    detectors_names.push_back("ORB");
    detectors.push_back(cv::ORB::create());
}

void MainController::initDescriptors()
{
    descriptors_names.push_back("SIFT");
    descriptors.push_back(cv::xfeatures2d::SIFT::create());
    norm_types.push_back(cv::NORM_L2SQR);

    descriptors_names.push_back("SURF");
    descriptors.push_back(cv::xfeatures2d::SURF::create());
    norm_types.push_back(cv::NORM_L2SQR);

    descriptors_names.push_back("KAZE");
    descriptors.push_back(cv::KAZE::create());
    norm_types.push_back(cv::NORM_L2SQR);

    descriptors_names.push_back("AKAZE");
    descriptors.push_back(cv::AKAZE::create());
    norm_types.push_back(cv::NORM_L2SQR);

    descriptors_names.push_back("BRISK");
    descriptors.push_back(cv::BRISK::create());
    norm_types.push_back(cv::NORM_HAMMING);

    descriptors_names.push_back("ORB");
    descriptors.push_back(cv::ORB::create());
    norm_types.push_back(cv::NORM_HAMMING);

    descriptors_names.push_back("FREAK");
    descriptors.push_back(cv::xfeatures2d::FREAK::create());
    norm_types.push_back(cv::NORM_HAMMING);
}

void MainController::clear()
{
    for (auto &selected_frames: trajectories_selected_frames)
    {
        selected_frames.clear();
    }
}

Trajectory MainController::loadTrjFromCsv(string csv_filename)
{        
    Trajectory trj;

    auto parsed_csv = utils::csvtools::read_csv(csv_filename);

    size_t found = csv_filename.find_last_of("/\\");
    string folder = csv_filename.substr(0,found);

    for (auto &row : parsed_csv)
    {
        if (row[0] == "Path")
        {//column names
            continue;
        }
        row[0] = folder + "/" + row[0];
        Map frame = loadMapFromRow(row);
        trj.pushBackFrame(frame);
    }

    return trj;
}

Map MainController::loadMapFromRow(std::vector<string> params)
{
    //need for atof, because '.' is not delimeter of float part
    char *saved_locale;
    saved_locale = setlocale(LC_NUMERIC, "C");


    double x_m = atof(params[1].c_str()); //QString::fromStdString(params[1]).toDouble();
    double y_m = atof(params[2].c_str()); //QString::fromStdString(params[2]).toDouble();

    double angle = atof(params[3].c_str()); //QString::fromStdString(params[3]).toDouble();
    double m_per_px = atof(params[4].c_str()); //QString::fromStdString(params[4]).toDouble();


    setlocale(LC_NUMERIC, saved_locale);

    return Map(params[0], x_m, y_m, angle, m_per_px);
}
