#pragma once

#include <image_transport/image_transport.h>
#include <ros/ros.h>

#include <ouster_ros/PacketMsg.h>

namespace ouster_ros {
namespace OS1 {

class Decoder {
 public:
  explicit Decoder(const ros::NodeHandle& pnh);

  Decoder(const Decoder&) = delete;
  Decoder operator=(const Decoder&) = delete;

  using PacketMsg = ouster_ros::PacketMsg;

  void LidarPacketCb(const PacketMsg& packet);
  void ImuPacketCb(const PacketMsg& packet);

 private:
  ros::NodeHandle pnh_;
  image_transport::ImageTransport it_;
  ros::Subscriber lidar_packet_sub_, imu_packet_sub_;
  ros::Publisher lidar_pub_, imu_pub_;
  image_transport::CameraPublisher camera_pub_;
};

}  // namespace OS1
}  // namespace ouster_ros
