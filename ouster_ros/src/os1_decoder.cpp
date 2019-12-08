#include "ouster_ros/os1_decoder.h"
#include "ouster/os1_util.h"
#include "ouster_ros/OS1ConfigSrv.h"

#include <cv_bridge/cv_bridge.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/point_cloud.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>

namespace ouster_ros {
namespace OS1 {

using namespace ouster::OS1;
using namespace sensor_msgs;
using PointT = pcl::PointXYZI;
using CloudT = pcl::PointCloud<PointT>;

static constexpr double deg2rad(double deg) { return deg * M_PI / 180.0; }
static constexpr double rad2deg(double rad) { return rad * 180.0 / M_PI; }
static constexpr auto kNaNF = std::numeric_limits<float>::quiet_NaN();
static constexpr float range_factor = 0.001f;

/// Convert a vector of double from deg to rad
void TransformDeg2RadInPlace(std::vector<double>& vec) {
  std::transform(vec.begin(), vec.end(), vec.begin(), deg2rad);
}

/// Convert image to point cloud
CloudT::Ptr ToCloud(const ImageConstPtr& image_msg, const CameraInfo& cinfo_msg,
                    bool organized);

Decoder::Decoder(const ros::NodeHandle& pnh) : pnh_(pnh), it_(pnh) {
  server_.setCallback(boost::bind(&Decoder::ConfigCb, this, _1, _2));

  // Call service to retrieve sensor information
  OS1ConfigSrv os1_srv;
  auto client = pnh_.serviceClient<ouster_ros::OS1ConfigSrv>("os1_config");
  client.waitForExistence();

  // Beam altitude angles go from top to bottom
  // Note these are all degrees
  if (client.call(os1_srv)) {
    ROS_INFO("Reading sensor info from os1 config");
    const auto& cfg = os1_srv.response;
    info_.beam_altitude_angles = cfg.beam_altitude_angles;
    info_.beam_azimuth_angles = cfg.beam_azimuth_angles;
    info_.imu_to_sensor_transform = cfg.imu_to_sensor_transform;
    info_.lidar_to_sensor_transform = cfg.lidar_to_sensor_transform;
    info_.mode = lidar_mode_of_string(cfg.lidar_mode);
    info_.hostname = cfg.hostname;
  } else {
    ROS_WARN("Calling os1 config service failed, revert to default");
    info_.beam_altitude_angles = beam_altitude_angles;
    info_.beam_azimuth_angles = beam_azimuth_angles;
    info_.imu_to_sensor_transform = imu_to_sensor_transform;
    info_.lidar_to_sensor_transform = lidar_to_sensor_transform;
    info_.mode = MODE_1024x10;
    info_.hostname = "UNKNOWN";
  }

  // Convert all angles to rad
  TransformDeg2RadInPlace(info_.beam_altitude_angles);
  TransformDeg2RadInPlace(info_.beam_azimuth_angles);

  ROS_INFO("Hostname: %s", info_.hostname.c_str());
  ROS_INFO("Lidar mode: %s", to_string(info_.mode).c_str());
}

void Decoder::LidarPacketCb(const PacketMsg& packet_msg) {
  //  ROS_DEBUG_THROTTLE(1, "Lidar packet size %zu", packet_msg.buf.size());
  buffer_.push_back(packet_msg);

  const auto curr_width = buffer_.size() * columns_per_buffer;

  if (curr_width < config_.image_width) {
    return;
  }

  // We have enough buffer decode
  ROS_DEBUG("Got enough packets %zu, ready to publish", buffer_.size());
  // [range, reflectivity, azimuth, (noise)]
  cv::Mat image = cv::Mat(pixels_per_column, curr_width, CV_32FC3,
                          cv::Scalar(kNaNF));  // all NaNs
  ROS_DEBUG("Image: %d x %d x %d", image.rows, image.cols, image.channels());

  for (int ibuf = 0; ibuf < buffer_.size(); ++ibuf) {
    const PacketMsg& packet = buffer_[ibuf];
    // Decode each packet
    const uint8_t* packet_buf = packet.buf.data();
    // Decode each azimuth block
    for (int icol = 0; icol < columns_per_buffer; ++icol) {
      const uint8_t* col_buf = nth_col(icol, packet_buf);
      const uint16_t m_id = col_measurement_id(col_buf);
      const uint16_t f_id = col_frame_id(col_buf);
      const bool valid = col_valid(col_buf) == 0xffffffff;
      // If a packet is bad (not valid) measurement_id, encoder count, range,
      // and reflectivity will also be 0

      // drop invalid data in case of misconfiguration
      if (!valid) {
        ROS_DEBUG("Got invalid data block");
        continue;
      }

      const int col = ibuf * columns_per_buffer + icol;
      const float theta0 = col_h_angle(col_buf);  // rad

      // Decode each beam
      for (uint8_t ipx = 0; ipx < pixels_per_column; ipx++) {
        const uint8_t* px_buf = nth_px(ipx, col_buf);
        const uint32_t range = px_range(px_buf);

        auto& v = image.at<cv::Vec3f>(ipx, col);
        v[0] = range * range_factor;
        v[1] = px_reflectivity(px_buf);
        v[2] = theta0 + info_.beam_azimuth_angles[ipx];
      }
    }
  }

  std_msgs::Header header;
  header.frame_id = "os1_lidar";  // TODO

  const ImagePtr image_msg =
      cv_bridge::CvImage(header, "32FC3", image).toImageMsg();
  const CameraInfoPtr cinfo_msg(new CameraInfo);
  cinfo_msg->header = header;
  cinfo_msg->height = image_msg->height;
  cinfo_msg->width = image_msg->width;
  cinfo_msg->D = info_.beam_altitude_angles;

  camera_pub_.publish(image_msg, cinfo_msg);
  cloud_pub_.publish(ToCloud(image_msg, *cinfo_msg, config_.organized));

  buffer_.clear();
}

void Decoder::ImuPacketCb(const PacketMsg& packet) {
  //  ROS_DEBUG_THROTTLE(1, "Imu packet size %zu", packet.buf.size());
}

void Decoder::ConfigCb(OusterOS1Config& config, int level) {
  // min_range should <= max_range
  config.min_range = std::min(config.min_range, config.max_range);

  // image_width is a multiple of columns_per_buffer
  config.image_width /= columns_per_buffer;
  config.image_width *= columns_per_buffer;

  ROS_INFO(
      "Reconfigure Request: min_range: %f, max_range: %f, image_width: %d, "
      "organized: %s, full_sweep: %s",
      config.min_range, config.max_range, config.image_width,
      config.organized ? "True" : "False",
      config.full_sweep ? "True" : "False");

  config_ = config;
  buffer_.clear();
  buffer_.reserve(config_.image_width);

  // Initialization
  if (level < 0) {
    // IO
    ROS_INFO("Initialize ROS subscriber/publisher");
    imu_packet_sub_ =
        pnh_.subscribe("imu_packets", 100, &Decoder::ImuPacketCb, this);
    lidar_packet_sub_ =
        pnh_.subscribe("lidar_packets", 2048, &Decoder::LidarPacketCb, this);

    imu_pub_ = pnh_.advertise<Imu>("imu", 100);
    camera_pub_ = it_.advertiseCamera("image", 10);
    cloud_pub_ = pnh_.advertise<PointCloud2>("cloud", 10);
    ROS_INFO("Decoder initialized");
  }
}

CloudT::Ptr ToCloud(const ImageConstPtr& image_msg, const CameraInfo& cinfo_msg,
                    bool organized) {
  CloudT::Ptr cloud_ptr(new CloudT);
  CloudT& cloud = *cloud_ptr;

  const auto image = cv_bridge::toCvShare(image_msg)->image;
  const auto& altitude_angles = cinfo_msg.D;

  cloud.header = pcl_conversions::toPCL(image_msg->header);
  cloud.reserve(image.total());

  for (int r = 0; r < image.rows; ++r) {
    const auto* const row_ptr = image.ptr<cv::Vec3f>(r);
    // Because image row 0 is the highest laser point
    const auto phi = altitude_angles[r];
    const auto cos_phi = std::cos(phi);
    const auto sin_phi = std::sin(phi);

    for (int c = 0; c < image.cols; ++c) {
      const cv::Vec3f& data = row_ptr[c];  // [range, reflectivity, azimuth]

      PointT p;
      if (std::isnan(data[0])) {
        if (organized) {
          p.x = p.y = p.z = p.intensity = kNaNF;
          cloud.points.push_back(p);
        }
      } else {
        // p.23 lidar range data to xyz lidar coordinate frame
        // x = d * cos(phi) * cos(theta);
        // y = d * cos(phi) * sin(theta);
        // z = d * sin(phi)
        const auto theta = data[2];
        const auto d = data[0];
        const auto x = d * cos_phi * std::cos(theta);
        const auto y = d * cos_phi * std::sin(theta);
        const auto z = d * sin_phi;

        p.x = x;
        p.y = -y;
        p.z = z;
        p.intensity = data[1];

        cloud.points.push_back(p);
      }
    }
  }

  if (organized) {
    cloud.width = image.cols;
    cloud.height = image.rows;
  } else {
    cloud.width = cloud.size();
    cloud.height = 1;
  }

  return cloud_ptr;
}

}  // namespace OS1
}  // namespace ouster_ros

int main(int argc, char** argv) {
  ros::init(argc, argv, "ouster_os1_decoder");
  ros::NodeHandle pnh("~");

  ouster_ros::OS1::Decoder node(pnh);
  ros::spin();
}
