#include <box_detector/box_detector.hpp>

namespace interiit21::box_detection {

void BoxDetectorNode::init(ros::NodeHandle &nh) {

  img_sub_ =
      nh.subscribe("image_raw", 1, &BoxDetectorNode::imageCallback, this);
  odom_sub_ = nh.subscribe("odom", 1, &BoxDetectorNode::odomCallback, this);
  pose_pub_ = nh.advertise<geometry_msgs::PoseStamped>("pose", 10);
  contour_pub_ = nh.advertise<sensor_msgs::Image>("contours", 10);
  msg_pub_ = nh.advertise<std_msgs::String>("message", 10);
  landing_client_ =
      nh.serviceClient<mavros_msgs::CommandTOL>("mavros/cmd/land");

  ros::NodeHandle nh_private("~");

  // set parameters
  nh_private.getParam("camera_matrix", camera_matrix_);
  nh_private.getParam("cam_to_quad_rot", camera_to_quad_matrix_);
  nh_private.getParam("t_cam", camera_translation_);
  nh_private.getParam("verbose", debug_);

  arrayToMatrixConversion();
  cameraMatrix = cv::Mat(3, 3, CV_32F, camera_matrix);
  distCoeffs = cv::Mat::zeros(1, 5, CV_32F);

  centre_(0) = -1;
  centre_(1) = -1;
  marker_detected_ = false;
  msg_ = "Marker ID: none, looking for marker";
}

void BoxDetectorNode::run() {
  detectArucoMarker();
  if (marker_detected_) {
    findGlobalCoordinates();
    landing();
  }
  msg_pub_.publish(msg_);
}

void BoxDetectorNode::arrayToMatrixConversion() {
  for (int i = 0; i < 3; i++) {
    camera_translation_vector_(i) = camera_translation_[i];
    for (int j = 0; j < 3; j++) {
      cameraToQuadMatrix(i, j) = camera_to_quad_matrix_[3 * i + j];
      camera_matrix[3 * i + j] = camera_matrix_[3 * i + j];
    }
  }
}

void BoxDetectorNode::landing() {
  if (isnan(global_coordinates_(0)) || isnan(global_coordinates_(1)) ||
      isnan(global_coordinates_(2))) {
    return;
  }
  pose_.pose.position.x = global_coordinates_(0);
  pose_.pose.position.y = global_coordinates_(1);
  pose_.pose.orientation = odom_.pose.pose.orientation;
  if (odom_.pose.pose.position.z > 2) {
    pose_.pose.position.z = odom_.pose.pose.position.z - 0.4;
    pose_pub_.publish(pose_);
    std::cout << odom_.pose.pose.position.z;
  } else if (odom_.pose.pose.position.z <= 2) {
    if (fabs(global_coordinates_(0) - odom_.pose.pose.position.x) <= 0.25 &&
        fabs(global_coordinates_(1) - odom_.pose.pose.position.y) <= 0.25) {
      // call lander
      msg_ = "Marker ID : 0, Landed";
      landing_client_.call(land_cmd_);
    } else {
      pose_.pose.position.z = odom_.pose.pose.position.z;
      pose_pub_.publish(pose_);
    }
  }
}

void BoxDetectorNode::odomCallback(const nav_msgs::Odometry &msg) {
  odom_ = msg;
  tf::Quaternion q(msg.pose.pose.orientation.x, msg.pose.pose.orientation.y,
                   msg.pose.pose.orientation.z, msg.pose.pose.orientation.w);
  Eigen::Quaterniond quat = Eigen::Quaterniond(q.w(), q.x(), q.y(), q.z());
  quadOrientationMatrix = quat.normalized().toRotationMatrix().inverse();
  translation_ =
      Eigen::Vector3d(msg.pose.pose.position.x, msg.pose.pose.position.y,
                      msg.pose.pose.position.z);
}

void BoxDetectorNode::detectArucoMarker() {
  if (img_.empty()) {
    return;
  };

  cv::Ptr<cv::aruco::DetectorParameters> parameters =
      cv::aruco::DetectorParameters::create();
  cv::Ptr<cv::aruco::Dictionary> dictionary =
      cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_1000);
  cv::aruco::detectMarkers(img_, dictionary, markerCorners, markerIds,
                           parameters, rejectedCandidates);

  outputImage = img_.clone();
  cv::aruco::drawDetectedMarkers(outputImage, markerCorners, markerIds);
  marker_detected_ = false;
  for (int i = 0; i < markerIds.size(); i++) {
    if (markerIds[i] == 0) {
      marker_detected_ = true;
      index_ = i;
      centre_(0) = (markerCorners[i][0].x + markerCorners[i][1].x +
                    markerCorners[i][2].x + markerCorners[i][3].x) /
                   4;
      centre_(1) = (markerCorners[i][0].y + markerCorners[i][1].y +
                    markerCorners[i][2].y + markerCorners[i][3].y) /
                   4;
      cv::circle(outputImage, cv::Point(centre_(0), centre_(1)), 4.0,
                 cv::Scalar(0, 0, 255), 1, 8);
    }
  }
  contour_msg =
      cv_bridge::CvImage(std_msgs::Header(), "bgr8", outputImage).toImageMsg();
  contour_pub_.publish(contour_msg);
}

void BoxDetectorNode::findGlobalCoordinates() {
  cv::aruco::estimatePoseSingleMarkers(markerCorners, 0.4, cameraMatrix,
                                       distCoeffs, rvecs, tvecs);
  cv::Vec3d tvec = tvecs[index_];
  Eigen::Vector3d camera_frame_coordinates(tvec[0], tvec[1], tvec[2]);
  Eigen::Vector3d coordinates_quad_frame =
      cameraToQuadMatrix * camera_frame_coordinates +
      camera_translation_vector_;
  global_coordinates_ =
      quadOrientationMatrix * coordinates_quad_frame + translation_;

  if (debug_) {
    std::cout << "-------------------------------------------------------------"
              << std::endl;
    std::cout << "Required Marker Detected"
              << "\n";
    std::cout << "global coordinates: " << global_coordinates_(0) << " "
              << global_coordinates_(1) << " " << global_coordinates_(2)
              << std::endl;
    std::cout << "centre_coordinates: " << centre_(0) << " " << centre_(1)
              << std::endl;
    std::cout << "camera frame coordinates: " << tvecs[0][0] << " "
              << tvecs[0][1] << " " << tvecs[0][2] << std::endl;
    std::cout << "-------------------------------------------------------------"
              << std::endl;
  }
}

void BoxDetectorNode::imageCallback(const sensor_msgs::ImageConstPtr &msg) {
  cv_bridge::CvImagePtr cv_ptr_;

  try {
    cv_ptr_ = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
  } catch (cv_bridge::Exception &e) {
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }

  img_ = cv_ptr_->image;
}

} // namespace interiit21::box_detection
