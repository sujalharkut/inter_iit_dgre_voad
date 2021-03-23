#include <active_planner/local_planner.hpp>

namespace local_planner {

LocalPlanner::LocalPlanner(ros::NodeHandle& nh, ros::NodeHandle& nh_private)
    : pathfinder_(nh, nh_private)
    , evaluator_(nh, nh_private)
    , active_(false)
    , exit_(false)
    , verbose_(false)
    , const_yaw_(3.14) {
    nh_private.getParam("visualize_planner", visualize_);
    nh_private.getParam("robot_radius", robot_radius_);
    nh_private.getParam("voxel_size", voxel_size_);
    nh_private.getParam("verbose_planner", verbose_);

    ROS_INFO_STREAM(verbose_);
    ROS_INFO_STREAM(visualize_);
    verbose_ = true;
    visualize_ = true;

    odom_sub_ = nh.subscribe("odometry", 1, &LocalPlanner::odometryCallback, this);
    command_pub_ = nh.advertise<geometry_msgs::PoseStamped>("command/pose", 1);
    activate_server_ = nh.advertiseService("activate", &LocalPlanner::activateCallback, this);
    exit_server_ = nh.advertiseService("shutdown", &LocalPlanner::exitCallback, this);

    if (visualize_) {
        visualizer_.init(nh, nh_private);
        visualizer_.createPublisher("occupied_path");
        visualizer_.createPublisher("free_path");
        visualizer_.createPublisher("trajectory");
    }
}

geometry_msgs::Point LocalPlanner::convertEigenToGeometryMsg(const Eigen::Vector3d& point) {
    geometry_msgs::Point point_msg;

    point_msg.x = point.x();
    point_msg.y = point.y();
    point_msg.z = point.z();

    return point_msg;
}

Eigen::Vector3d LocalPlanner::getBestFrontier() {
    geometry_msgs::Point curr_pos = odometry_.pose.pose.position;
    double yaw = mav_msgs::yawFromQuaternion(mav_msgs::quaternionFromMsg(odometry_.pose.pose.orientation));
    double max_distance = DBL_MIN;
    Eigen::Vector3d best_f = Eigen::Vector3d::Zero();
    for (auto frontier : frontiers_) {
        Eigen::Vector3d vector = (frontier.center - Eigen::Vector3d(curr_pos.x, curr_pos.y, curr_pos.z));
        double distance = cos(yaw) * vector.x() + sin(yaw) * vector.y();
        ROS_INFO_STREAM(distance);
        if (distance > max_distance) {
            if (visited_frontiers_.find(getHash(frontier.center)) == visited_frontiers_.end()) {
                max_distance = distance;
                best_f = frontier.center;
            }
        }
    }
    ROS_INFO_STREAM("BRUH: " << max_distance << " " << yaw);
    return best_f;
}

void LocalPlanner::run() {
    frontiers_.clear();
    trajectory_.clear();
    frontier_path_.clear(); 

    if (waypoint_queue_.empty()) {
        evaluator_.findFrontiers();
        frontiers_ = evaluator_.getFrontiers();

        if (verbose_){
            ROS_INFO_STREAM("Found " << frontiers_.size() << " frontiers");
        }
        
        Eigen::Vector3d waypoint = getBestFrontier();
        if(verbose_){
            ROS_WARN_STREAM(waypoint.norm());
        }

        if(waypoint.norm() < voxel_size_){
            if (verbose_) {
                ROS_WARN("No explorable frontier! Spinning around once.");
                visited_frontiers_[getHash(waypoint)] = waypoint;
            }

            geometry_msgs::PoseStamped turn_msg;
            turn_msg.header.stamp = ros::Time::now();

            geometry_msgs::Quaternion orig = odometry_.pose.pose.orientation;
            turn_msg.pose.position = odometry_.pose.pose.position;
            turn_msg.pose.orientation.x = orig.x;
            turn_msg.pose.orientation.y = orig.y;
            turn_msg.pose.orientation.z = -orig.w;
            turn_msg.pose.orientation.w = orig.z;
            command_pub_.publish(turn_msg);

            ros::Duration(1.0).sleep();
            ros::spinOnce();

            turn_msg.pose.orientation = orig;
            command_pub_.publish(turn_msg);
        } else {
            
            if (verbose_) {
                ROS_INFO_STREAM("Pursuing new frontier: \n" << waypoint);
            }
            waypoint_queue_.push(waypoint);
        }
    } else {
        Eigen::Vector3d waypt = waypoint_queue_.top();
        waypoint_queue_.pop();

        ros::spinOnce();
        mav_msgs::EigenOdometry start_odom;
        mav_msgs::eigenOdometryFromMsg(odometry_, &start_odom);

        frontier_path_.clear();
        trajectory_.clear();

        pathfinder_.findPath(start_odom.position_W, waypt);
        frontier_path_ = pathfinder_.getPath();
        trajectory_ = generateTrajectoryThroughWaypoints(frontier_path_);

        if (verbose_) {
            ROS_INFO_STREAM("Generated " << trajectory_.size() << " waypoints");
        }

        visited_frontiers_[getHash(waypt)] = waypt;
        if (trajectory_.empty()) {
            if(verbose_){
                ROS_INFO("Current frontier not feasible.");
                return;
            }
        }

        ros::Rate pub_rate(40);
        for (auto i = 0; i < trajectory_.size(); i++) {
            auto target = trajectory_[i];
            geometry_msgs::PoseStamped setpt;
            double temp = target.orientation_W_B.z();
            target.orientation_W_B.z() = -target.orientation_W_B.w();
            target.orientation_W_B.w() = temp;
            mav_msgs::msgPoseStampedFromEigenTrajectoryPoint(target, &setpt);
            command_pub_.publish(setpt);

            if (verbose_) {
                ROS_INFO("Published next waypoint!");
            }

            ros::spinOnce();
            bool feasible = true;

            while (ros::ok() && (norm(odometry_.pose.pose.position, convertEigenToGeometryMsg(target.position_W)) > voxel_size_)) {
                ros::spinOnce();
                mav_msgs::EigenOdometry start_odom;
                mav_msgs::eigenOdometryFromMsg(odometry_, &start_odom);

                if (checkForAbort(i, trajectory_)) {
                    if (verbose_) {
                        ROS_WARN("Aborting current trajectory...");
                    }

                    geometry_msgs::PoseStamped stop_pt;
                    ros::spinOnce();

                    stop_pt.pose = odometry_.pose.pose;
                    stop_pt.header.stamp = ros::Time::now();
                    command_pub_.publish(stop_pt);

                    feasible = false;
                    trajectory_.clear();
                    frontier_path_.clear();

                    Eigen::Vector3d waypoint = getBestFrontier();
                    if(waypoint.norm() > voxel_size_){
                        if (verbose_) {
                            ROS_INFO_STREAM("Pursuing new frontier: \n" << waypoint);
                        }
                        waypoint_queue_.push(waypoint);
                    }

                    break;
                }
                command_pub_.publish(setpt);
                pub_rate.sleep();
            }

            if (!feasible) {
                trajectory_.clear();
                frontier_path_.clear();
                ros::spinOnce();
                
                if(verbose_){
                    ROS_INFO("Aborted. Looking for next frontier");
                }
                
                visited_frontiers_[getHash(waypt)] = waypt;
                break;
            }
        }

        if(verbose_){
            ROS_INFO_STREAM("Looking for next frontier");
        }
    }
}

bool LocalPlanner::checkForAbort(const uint i, Trajectory& trajectory){
    bool need_abort = false;
    uint occupied = 0;

    std::vector<Eigen::Vector3d> free_points, occ_points;

    double distance = 0.0;
    for (auto j = i; j < (i+4) && j < trajectory.size(); j++) {
        if (pathfinder_.getMapDistance(trajectory[j].position_W, distance) && distance < robot_radius_) {
            occupied++;
            if (visualize_) {
                occ_points.push_back(trajectory[j].position_W);
            }
        } else if (visualize_) {
            free_points.push_back(trajectory[j].position_W);
        }
    }

    need_abort = (occupied > 0); 

    if (visualize_) {
        visualizer_.visualizePoints("occupied_path", occ_points, "map", Visualizer::ColorType::RED, 1);
        visualizer_.visualizePoints("free_path", free_points, "map", Visualizer::ColorType::GREEN, 0.5);
    }

    return need_abort;
}

Trajectory LocalPlanner::generateTrajectoryThroughWaypoints(const Path& waypoints) {
    Trajectory traj;
    if (waypoints.empty()) {
        return traj;
    }

    for (auto i = 0; i < waypoints.size(); i++) {
        mav_msgs::EigenTrajectoryPoint traj_pt;
        traj_pt.position_W = waypoints[i];
        traj.push_back(traj_pt);
    }

    applyYawToTrajectory(traj);
    if (visualize_) {
        visualizer_.visualizeTrajectory("trajectory", traj, "map", Visualizer::ColorType::BLACK, 0.2);
    }
    return traj;
}

void LocalPlanner::applyYawToTrajectory(Trajectory& trajectory, const YawPolicy& policy) {
    if (trajectory.size() < 2) {
        return;
    }
    double last_yaw = mav_msgs::yawFromQuaternion(mav_msgs::quaternionFromMsg(odometry_.pose.pose.orientation));

    if (policy == YawPolicy::POINT_FACING) {
        for (auto i = 0; i < trajectory.size() - 1; i++) {
            Eigen::Vector3d heading = (trajectory[i+1].position_W - trajectory[i].position_W).normalized();
            Eigen::Vector3d plane_heading(heading.x(), heading.y(), 0);
            double desired_yaw = 0.0;
            if (std::fabs(heading.x()) > 1e-4 || std::fabs(heading.y()) > 1e-4) {
                desired_yaw = std::acos(heading.dot(plane_heading.normalized()));
            } else {
                desired_yaw = last_yaw;
            }
            trajectory[i].setFromYaw(desired_yaw);
            last_yaw = desired_yaw;
        }
    } else if (policy == YawPolicy::FOLLOW_VELOCITY) {
        const double minVelocityNorm = 0.1;
        for (auto i = 0; i < trajectory.size(); i++) {
            Eigen::Vector3d velocity_xy = trajectory[i].velocity_W;
            velocity_xy.z() = 0;
            if (velocity_xy.norm() > minVelocityNorm) {
                double desired_yaw = atan2(velocity_xy.y(), velocity_xy.x());
                trajectory[i].setFromYaw(desired_yaw);
                last_yaw = desired_yaw;
            } else {
                double desired_yaw = last_yaw;
                auto j = i + 1;
                while (j < trajectory.size() && velocity_xy.norm() < minVelocityNorm) {
                    velocity_xy = trajectory[j].velocity_W;
                    velocity_xy.z() = 0;
                    j++;
                }
                if (velocity_xy.norm() > minVelocityNorm) {
                    desired_yaw = atan2(velocity_xy.y(), velocity_xy.x());
                }
                trajectory[i].setFromYaw(desired_yaw);
                last_yaw = desired_yaw;
            }
        }
    } else if (policy == YawPolicy::ANTICIPATE_VELOCITY) {
        const double minVelocityNorm = 0.1;
        double initial_yaw = last_yaw;
        for (auto i = trajectory.size(); i > 0; i--) {
            Eigen::Vector3d velocity_xy = trajectory[i].velocity_W;
            velocity_xy.z() = 0;
            if (velocity_xy.norm() > minVelocityNorm) {
                double desired_yaw = atan2(velocity_xy.y(), velocity_xy.x());
                trajectory[i].setFromYaw(desired_yaw);
                last_yaw = desired_yaw;
            } else {
                double desired_yaw = last_yaw;
                for (auto j = i; j > 0 && velocity_xy.norm() < minVelocityNorm; j--) {
                    velocity_xy = trajectory[j - 1].velocity_W;
                    velocity_xy.z() = 0;
                }
                if (velocity_xy.norm() > minVelocityNorm) {
                    desired_yaw = atan2(velocity_xy.y(), velocity_xy.x());
                }
                trajectory[i].setFromYaw(desired_yaw);
                last_yaw = desired_yaw;
            }
        }
        trajectory[0].setFromYaw(initial_yaw);
    } else if (policy == YawPolicy::CONSTANT) {
        for (auto i = 0; i < trajectory.size(); i++) {
            trajectory[i].setFromYaw(const_yaw_);
        }
    }
}

}  // namespace local_planner