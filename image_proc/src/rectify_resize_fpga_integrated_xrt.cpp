/*
   @@@@@@@@@@@@@@@@@@@@
   @@@@@@@@@&@@@&&@@@@@
   @@@@@ @@  @@    @@@@
   @@@@@ @@  @@    @@@@
   @@@@@ @@  @@    @@@@ Copyright (c) 2023, Acceleration Robotics®
   @@@@@ @@  @@    @@@@ Author: Alejandra Martínez Fariña <alex@accelerationrobotics.com>
   @@@@@ @@  @@    @@@@ 
   @@@@@@@@@&@@@@@@@@@@
   @@@@@@@@@@@@@@@@@@@@

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. 
*/

/*
      ____  ____
     /   /\/   /
    /___/  \  /   Copyright (c) 2021, Xilinx®.
    \   \   \/    Author: Víctor Mayoral Vilches <victorma@xilinx.com>
     \   \
     /   /        Licensed under the Apache License, Version 2.0 (the "License");
    /___/   /\    you may not use this file except in compliance with the License.
    \   \  /  \   You may obtain a copy of the License at
     \___\/\___\            http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

    Inspired by rectify.cpp authored by Willow Garage, Inc., Andreas Klintberg,
      Joshua Whitley. Inspired also by PinholeCameraModel class.
*/

#include <rclcpp/rclcpp.hpp>
#include <cv_bridge/cv_bridge.h>
#include <image_geometry/pinhole_camera_model.h>
#include <image_geometry/pinhole_camera_model.h>
#include <image_transport/image_transport.hpp>
#include <vitis_common/common/xf_headers.hpp>
#include <vitis_common/common/utilities.hpp>

#include <thread>
#include <memory>
#include <vector>
#include <chrono>

#include "image_proc/rectify_resize_fpga_integrated_xrt.hpp"
#include "tracetools_image_pipeline/tracetools.h"
#include <rclcpp/serialization.hpp>

namespace image_geometry
{

// From pinhole_camera_model.cpp
enum DistortionState { NONE, CALIBRATED, UNKNOWN };

struct PinholeCameraModelFPGAIntegratedXRT::Cache
{
  DistortionState distortion_state;

  cv::Mat_<double> K_binned, P_binned; // NOLINT, Binning applied, but not cropping

  mutable bool full_maps_dirty;
  mutable cv::Mat full_map1, full_map2;

  mutable bool reduced_maps_dirty;
  mutable cv::Mat reduced_map1, reduced_map2;

  mutable bool rectified_roi_dirty;
  mutable cv::Rect rectified_roi;

  Cache()
    : full_maps_dirty(true),
      reduced_maps_dirty(true),
      rectified_roi_dirty(true)
  {
  }
};

PinholeCameraModelFPGAIntegratedXRT::PinholeCameraModelFPGAIntegratedXRT()
{
  // XRT approach
  // create kernel
  device = xrt::device(0);  // Open the device, default to 0
  // TODO: generalize this using launch extra_args for composable Nodes
  // see https://github.com/ros2/launch_ros/blob/master/launch_ros/launch_ros/descriptions/composable_node.py#L45
  //
  // use "extra_arguments" from ComposableNode and propagate the value to this class constructor
  uuid = device.load_xclbin("/lib/firmware/xilinx/image_proc_integrated/image_proc_integrated.xclbin");
  krnl_rectify = xrt::kernel(device, uuid, "rectify_resize_accel");

}

void PinholeCameraModelFPGAIntegratedXRT::rectifyResizeImageFPGA(
  const cv::Mat& raw,
  cv::Mat& rectified,
  sensor_msgs::msg::CameraInfo::SharedPtr dst_info_msg,
  sensor_msgs::msg::Image::ConstSharedPtr image_msg,
  sensor_msgs::msg::CameraInfo::ConstSharedPtr info_msg,
  bool gray) const
{
  //Serialize the Image and CameraInfo messages
  rclcpp::SerializedMessage serialized_data_img;
  rclcpp::Serialization<sensor_msgs::msg::Image> image_serialization;
  const void* image_ptr = reinterpret_cast<const void*>(image_msg.get());
  image_serialization.serialize_message(image_ptr, &serialized_data_img);
  size_t image_msg_size = serialized_data_img.get_rcl_serialized_message().buffer_length;
  
  rclcpp::SerializedMessage serialized_data_info;
  rclcpp::Serialization<sensor_msgs::msg::CameraInfo> info_serialization;
  const void* info_ptr = reinterpret_cast<const void*>(info_msg.get());
  info_serialization.serialize_message(info_ptr, &serialized_data_info);
  size_t info_msg_size = serialized_data_info.get_rcl_serialized_message().buffer_length;
  
  // Rectify and resize
  TRACEPOINT(
    image_proc_rectify_init,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*image_msg)),
    static_cast<const void *>(&(*info_msg)),
    image_msg->header.stamp.nanosec,
    image_msg->header.stamp.sec,
    image_msg_size,
    info_msg_size);

  assert(initialized());

  cv::Mat hls_remapped;
  switch (cache_->distortion_state) {
    case NONE:
      raw.copyTo(rectified);
      break;
    case CALIBRATED:
      {
        initRectificationMaps();

        cv::Mat map_x, map_y;
        std::vector<cv::Mat> channels_map;

        // if (cache_->reduced_map1.channels() == 2) {
        if (cache_->reduced_map1.type() == CV_32FC2 || cache_->reduced_map1.type() == CV_16SC2) {  // NOLINT
          cv::convertMaps(cache_->reduced_map1, cache_->reduced_map2, map_x, map_y, CV_32FC1);  // NOLINT
        } else if(cache_->reduced_map1.channels() == 1 && cache_->reduced_map2.channels() == 1) {   // NOLINT
          map_x = cache_->reduced_map1;
          map_y = cache_->reduced_map2;
        } else {
          throw Exception(
            "Unexpected amount of channels in warp maps: " +
            cache_->reduced_map1.channels());
        }

        //  REVIEW: Vitis Vision Library reviewed does not seem to offer
        //  capabilities to differentiate between CV_32F and CV_64F. It'd be
        //  nice to consult with the appropriate team specialized on this.
        //
        int channels = gray ? 1 : 3;
        size_t image_in_size_count = raw.rows * raw.cols * channels;
        size_t image_in_size_bytes = image_in_size_count * sizeof(unsigned char);
        // size_t image_in_size_bytes = raw.rows * raw.cols
        //                               * sizeof(unsigned char)
        //                               * channels;

        size_t map_in_size_count = raw.rows * raw.cols;
        size_t map_in_size_bytes = map_in_size_count * sizeof(float);
        // size_t map_in_size_bytes = raw.rows * raw.cols * sizeof(float);

        size_t image_out_size_count = dst_info_msg->height *
                                      dst_info_msg->width * channels;
        size_t image_out_size_bytes = image_out_size_count * sizeof(unsigned char);

        hls_remapped.create(
          cv::Size(dst_info_msg->width, dst_info_msg->height),
          raw.type());

        ///////////////////////////XRT///////////////////////////////////
        // Allocate the buffers in global memory
        auto buffer_inImage = xrt::bo(device,
                                      image_in_size_bytes,
                                      krnl_rectify.group_id(0));
        auto buffer_inMapX = xrt::bo(device,
                                     map_in_size_bytes,
                                     krnl_rectify.group_id(1));
        auto buffer_inMapY = xrt::bo(device,
                                     map_in_size_bytes,
                                     krnl_rectify.group_id(2));
        auto buffer_outImage = xrt::bo(device,
                                     image_out_size_bytes,
                                     krnl_rectify.group_id(3));

        // Map the contents of the buffer object into host memory
        auto buffer_inImage_map = buffer_inImage.map<unsigned char*>();
        auto buffer_inMapX_map = buffer_inMapX.map<float*>();
        auto buffer_inMapY_map = buffer_inMapY.map<float*>();
        auto buffer_outImage_map = buffer_outImage.map<unsigned char*>();

        // Initialize the buffers
        if (!raw.isContinuous()) {
          std::cout << "raw cv::Mat is not continuous" << std::endl;
          break;
        }
        // std::fill(buffer_inImage_map, buffer_inImage_map + image_in_size_count, 0);  // NOLINT
        // std::fill(buffer_inMapX_map, buffer_inMapX_map + map_in_size_count, 0);  // NOLINT
        // std::fill(buffer_inMapY_map, buffer_inMapY_map + map_in_size_count, 0);  // NOLINT
        std::fill(buffer_outImage_map, buffer_outImage_map + image_out_size_count, 0);  // NOLINT

        std::memcpy(buffer_inImage_map, raw.data, image_in_size_bytes);  // NOLINT
        std::memcpy(buffer_inMapX_map, map_x.data, map_in_size_bytes);  // NOLINT
        std::memcpy(buffer_inMapY_map, map_y.data, map_in_size_bytes);  // NOLINT        

        // Synchronize buffers with device side
        buffer_inImage.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        buffer_inMapX.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        buffer_inMapY.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        buffer_outImage.sync(XCL_BO_SYNC_BO_TO_DEVICE);

        // Set kernel arguments, run and wait for it to finish
        auto run = xrt::run(krnl_rectify);
        run.set_arg(0, buffer_inImage);
        run.set_arg(1, buffer_inMapX);
        run.set_arg(2, buffer_inMapY);
        run.set_arg(3, buffer_outImage);
        run.set_arg(4, raw.rows);
        run.set_arg(5, raw.cols);
        run.set_arg(6, dst_info_msg->height);
        run.set_arg(7, dst_info_msg->width);
        run.start();

        run.wait();

        // Get the output;
        buffer_outImage.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

        // TODO: if neccessary, assign to "rectified" a cv::Mat knowing that
        // buffer_outImage corresponds with the ".data" field of a cv::Mat
        hls_remapped.data = buffer_outImage_map;
        rectified = hls_remapped.clone();  // deep copy
        break;
      }
    default:
      assert(cache_->distortion_state == UNKNOWN);
      throw Exception("Cannot call rectifyImage when distortion is unknown.");
  }
  TRACEPOINT(
    image_proc_rectify_fini,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*image_msg)),
    static_cast<const void *>(&(*info_msg)),
    image_msg->header.stamp.nanosec,
    image_msg->header.stamp.sec,
    image_msg_size,
    info_msg_size);

}
}  // namespace image_geometry

namespace image_proc
{

RectifyResizeNodeFPGAXRT::RectifyResizeNodeFPGAXRT(
  const rclcpp::NodeOptions & options)
: Node("RectifyResizeNodeFPGAXRT", options)
{
  // Rectify params
  queue_size_ = this->declare_parameter("queue_size", 5);
  interpolation = this->declare_parameter("interpolation", 1);

  // Resize params
  use_scale_ = this->declare_parameter("use_scale", true);
  scale_height_ = this->declare_parameter("scale_height", 1.0);
  scale_width_ = this->declare_parameter("scale_width", 1.0);
  height_ = this->declare_parameter("height", -1);
  width_ = this->declare_parameter("width", -1);
  profile_ = this->declare_parameter("profile", true);

  // pub_rect_ = image_transport::create_publisher(this, "image_rect");
  pub_image_ = image_transport::create_camera_publisher(this, "resize");
  subscribeToCamera();
}

// Handles (un)subscribing when clients (un)subscribe
void RectifyResizeNodeFPGAXRT::subscribeToCamera()
{
  std::lock_guard<std::mutex> lock(connect_mutex_);

  /*
  *  SubscriberStatusCallback not yet implemented
  *
  if (pub_rect_.getNumSubscribers() == 0)
    sub_camera_.shutdown();
  else if (!sub_camera_)
  {
  */
  sub_camera_ = image_transport::create_camera_subscription(
    this, "image", std::bind(
      &RectifyResizeNodeFPGAXRT::imageCb,
      this, std::placeholders::_1, std::placeholders::_2), "raw");
  // }
}

void RectifyResizeNodeFPGAXRT::imageCb(
  const sensor_msgs::msg::Image::ConstSharedPtr & image_msg,
  const sensor_msgs::msg::CameraInfo::ConstSharedPtr & info_msg)
{
  //Serialize the Image and CameraInfo messages
  rclcpp::SerializedMessage serialized_data_img;
  rclcpp::Serialization<sensor_msgs::msg::Image> image_serialization;
  const void* image_ptr = reinterpret_cast<const void*>(image_msg.get());
  image_serialization.serialize_message(image_ptr, &serialized_data_img);
  size_t image_msg_size = serialized_data_img.get_rcl_serialized_message().buffer_length;
  
  rclcpp::SerializedMessage serialized_data_info;
  rclcpp::Serialization<sensor_msgs::msg::CameraInfo> info_serialization;
  const void* info_ptr = reinterpret_cast<const void*>(info_msg.get());
  info_serialization.serialize_message(info_ptr, &serialized_data_info);
  size_t info_msg_size = serialized_data_info.get_rcl_serialized_message().buffer_length;
  
  TRACEPOINT(
    image_proc_rectify_cb_init,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*image_msg)),
    static_cast<const void *>(&(*info_msg)),
    image_msg->header.stamp.nanosec,
    image_msg->header.stamp.sec,
    image_msg_size,
    info_msg_size);

  if (pub_image_.getNumSubscribers() < 1) {
    TRACEPOINT(
      image_proc_rectify_cb_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*image_msg)),
      static_cast<const void *>(&(*info_msg)),
      image_msg->header.stamp.nanosec,
      image_msg->header.stamp.sec,
      image_msg_size,
      info_msg_size);
    return;
  }

  // Verify camera is actually calibrated
  if (info_msg->k[0] == 0.0) {
    RCLCPP_ERROR(
      this->get_logger(), "Rectified topic '%s' requested  "
      "is uncalibrated", pub_image_.getTopic().c_str());
    TRACEPOINT(
      image_proc_rectify_cb_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*image_msg)),
      static_cast<const void *>(&(*info_msg)),
      image_msg->header.stamp.nanosec,
      image_msg->header.stamp.sec,
      image_msg_size,
      info_msg_size);
    return;
  }

  // If zero distortion, just pass the message along
  bool zero_distortion = true;

  for (size_t i = 0; i < info_msg->d.size(); ++i) {
    if (info_msg->d[i] != 0.0) {
      zero_distortion = false;
      break;
    }
  }

  // This will be true if D is empty/zero sized
  if (zero_distortion) {
    pub_image_.publish(*image_msg, *info_msg);
    TRACEPOINT(
      image_proc_rectify_cb_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*image_msg)),
      static_cast<const void *>(&(*info_msg)),
      image_msg->header.stamp.nanosec,
      image_msg->header.stamp.sec,
      image_msg_size,
      info_msg_size);
    return;
  }

  bool gray =
    (sensor_msgs::image_encodings::numChannels(image_msg->encoding) == 1);

  cv_bridge::CvImagePtr cv_ptr;
  try {
    cv_ptr = cv_bridge::toCvCopy(image_msg);
  } catch (cv_bridge::Exception & e) {
    RCLCPP_ERROR(this->get_logger(), "cv_bridge exception: %s", e.what());
    TRACEPOINT(
      image_proc_resize_cb_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*image_msg)),
      static_cast<const void *>(&(*info_msg)),
      image_msg->header.stamp.nanosec,
      image_msg->header.stamp.sec,
      image_msg_size,
      info_msg_size);
    return;
  }

  // Update the camera model
  model_.fromCameraInfo(info_msg);

  // Create cv::Mat views onto both buffers
  const cv::Mat image = cv_bridge::toCvShare(image_msg)->image;
  cv::Mat rect;

  // Prep<are output CameraInfo with desired dimensions
  sensor_msgs::msg::CameraInfo::SharedPtr dst_info_msg =
    std::make_shared<sensor_msgs::msg::CameraInfo>(*info_msg);

  double scale_y;
  double scale_x;

  if (use_scale_) {
    scale_y = scale_height_;
    scale_x = scale_width_;
    dst_info_msg->height = static_cast<int>(info_msg->height * scale_height_);
    dst_info_msg->width = static_cast<int>(info_msg->width * scale_width_);
  } else {
    scale_y = static_cast<double>(height_) / info_msg->height;
    scale_x = static_cast<double>(width_) / info_msg->width;
    dst_info_msg->height = height_;
    dst_info_msg->width = width_;
  }

  // Rescale the relevant entries of the intrinsic and extrinsic matrices
  dst_info_msg->k[0] = dst_info_msg->k[0] * scale_x;  // fx
  dst_info_msg->k[2] = dst_info_msg->k[2] * scale_x;  // cx
  dst_info_msg->k[4] = dst_info_msg->k[4] * scale_y;  // fy
  dst_info_msg->k[5] = dst_info_msg->k[5] * scale_y;  // cy

  dst_info_msg->p[0] = dst_info_msg->p[0] * scale_x;  // fx
  dst_info_msg->p[2] = dst_info_msg->p[2] * scale_x;  // cx
  dst_info_msg->p[3] = dst_info_msg->p[3] * scale_x;  // T
  dst_info_msg->p[5] = dst_info_msg->p[5] * scale_y;  // fy
  dst_info_msg->p[6] = dst_info_msg->p[6] * scale_y;  // cy

  // Rectify and resize
  model_.rectifyResizeImageFPGA(image,
                          rect,
                          dst_info_msg,
                          image_msg,
                          info_msg,
                          gray);


  // Set the output image
  cv_bridge::CvImage output_image;
  output_image.header = cv_ptr->header;
  output_image.encoding = cv_ptr->encoding;
  if (gray) {
    output_image.image =
          cv::Mat{
              static_cast<int>(dst_info_msg->height),
              static_cast<int>(dst_info_msg->width),
              CV_8UC1,
              rect.data
          };
  } else {
    output_image.image =
          cv::Mat{
              static_cast<int>(dst_info_msg->height),
              static_cast<int>(dst_info_msg->width),
              CV_8UC3,
              rect.data
          };
  }

  pub_image_.publish(*output_image.toImageMsg(), *dst_info_msg);
  TRACEPOINT(
    image_proc_rectify_cb_fini,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*image_msg)),
    static_cast<const void *>(&(*info_msg)),
    image_msg->header.stamp.nanosec,
    image_msg->header.stamp.sec,
    image_msg_size,
    info_msg_size);
}

}  // namespace image_proc

#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
// This acts as a sort of entry point, allowing the
// component to be discoverable when its library
// is being loaded into a running process.
RCLCPP_COMPONENTS_REGISTER_NODE(image_proc::RectifyResizeNodeFPGAXRT)
