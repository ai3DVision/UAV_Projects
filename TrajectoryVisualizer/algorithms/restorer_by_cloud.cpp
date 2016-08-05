#include "restorer_by_cloud.h"

#include <algorithm>
#include <iostream>

#include <opencv2/calib3d.hpp>

#include "transformator.h"

using namespace algorithmspkg;

RestorerByCloud::RestorerByCloud(FeatureBasedRestorer::DetectorPtr detector,
                                 FeatureBasedRestorer::DescriptorPtr descriptor,
                                 FeatureBasedRestorer::MatcherPtr matcher,
                                 size_t max_key_points_per_frame)
  : FeatureBasedRestorer(detector, descriptor, matcher,
                         max_key_points_per_frame)
{
}

void RestorerByCloud::addFrame(const cv::Mat &frame,
                               const cv::Point2f &pos,
                               double angle, double scale)
{
  frames_key_points.push_back(KeyPointsList());
  std::vector<cv::KeyPoint> &key_points = frames_key_points.back();

  getDetector()->detect(frame, frames_key_points.back());

  std::sort(key_points.begin(), key_points.end(),
            []( const cv::KeyPoint &left, const cv::KeyPoint &right ) ->
            bool { return left.response > right.response; });

  if (getMaxKeyPointsPerFrame() != 0 &&
      getMaxKeyPointsPerFrame() < key_points.size())
  {
    key_points.resize(getMaxKeyPointsPerFrame());
  }
  key_points.shrink_to_fit();



  cv::Mat descriptions;
  getDescriptor()->compute(frame, frames_key_points.back(),
                     descriptions);

  getMatcher()->add(descriptions);

  cv::Point2f image_center(frame.cols/2., frame.rows/2.);
  transformKeyPointsPosition(key_points, image_center,
                             pos, angle, scale);
}

void RestorerByCloud::addFrame(const cv::Point2f &image_center,
                               const KeyPointsList &key_points,
                               const cv::Mat &descriptions,
                               const cv::Point2f &pos,
                               double angle, double scale)
{
  frames_key_points.push_back(key_points);
  getMatcher()->add(descriptions);

  transformKeyPointsPosition(frames_key_points.back(), image_center,
                             pos, angle, scale);
}

double RestorerByCloud::recoverLocation(const cv::Point2f &frame_center,
                                        cv::Point2f &pos,
                                        double &angle, double &scale)
{
  rough_matches.clear();
  if (query_key_points.empty())
  {
    pos = cv::Point2f(0, 0);
    angle = scale = 0;
    return 0;
  }

  getMatcher()->match(query_descriptions, rough_matches);


  train_pts.clear();
  query_pts.clear();
  for (const cv::DMatch &match: rough_matches)
  {
    train_pts.push_back(frames_key_points[match.imgIdx][match.trainIdx].pt);
    query_pts.push_back(query_key_points[match.queryIdx].pt);
  }

  homography_mask.clear();
  homography = cv::findHomography(query_pts, train_pts,
                                         cv::RANSAC, 3, homography_mask);

  cv::Point2f shift;
  Transformator::getParams(homography, shift, angle, scale);

  pos =  Transformator::transform(frame_center, homography);

  matches.clear();
  for (size_t i = 0; i < homography_mask.size(); i++)
  {
    if (homography_mask[i])
    {
      matches.push_back(rough_matches[i]);
    }
  }

  return calculateConfidence();
}


double RestorerByCloud::calculateConfidence() noexcept
{
  if (!rough_matches.empty())
  {
    return matches.size() / double(rough_matches.size());
  }
  else
  {
    return 0;
  }
}

size_t RestorerByCloud::getFramesCount() const
{
  return frames_key_points.size();
}

const FeatureBasedRestorer::KeyPointsList&
            RestorerByCloud::getFrameKeyPoints(size_t frame_num) const
{
  return frames_key_points[frame_num];
}

const cv::Mat&
            RestorerByCloud::getFrameDescriptions(size_t frame_num) const
{
  return getMatcher()->getTrainDescriptors()[frame_num];
}

void RestorerByCloud::save(std::string filename)
{
  std::clog << "No implementation for saving restorerByCloud" << std::endl;
}

void RestorerByCloud::load(std::string filename)
{
  std::clog << "No implementation for loading restorerByCloud" << std::endl;
}
