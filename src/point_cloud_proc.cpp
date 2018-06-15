#include <point_cloud_proc/point_cloud_proc.h>

PointCloudProc::PointCloudProc(ros::NodeHandle n, bool debug) :
        nh_(n), debug_(debug), cloud_transformed_(new CloudT), cloud_filtered_(new CloudT),
        cloud_hull_(new CloudT), cloud_tabletop_(new CloudT) {


    std::string pkg_path = ros::package::getPath("point_cloud_proc");
    std::string config_path = pkg_path + "/config/robocup_montreal.yaml";

    YAML::Node parameters = YAML::LoadFile(config_path);

    // General parameters
    point_cloud_topic_ = parameters["point_cloud_topic"].as<std::string>();
    fixed_frame_ = parameters["fixed_frame"].as<std::string>();

    // Segmentation parameters
    eps_angle_ = parameters["segmentation"]["sac_eps_angle"].as<float>();
    single_dist_thresh_ = parameters["segmentation"]["sac_dist_thresh_single"].as<float>();
    multi_dist_thresh_ = parameters["segmentation"]["sac_dist_thresh_multi"].as<float>();
    min_plane_size_= parameters["segmentation"]["sac_min_plane_size"].as<int>();
    max_iter_ = parameters["segmentation"]["sac_max_iter"].as<int>();
    k_search_ = parameters["segmentation"]["ne_k_search"].as<int>();
    cluster_tol_ = parameters["segmentation"]["ec_cluster_tol"].as<float>();
    min_cluster_size_ = parameters["segmentation"]["ec_min_cluster_size"].as<int>();
    max_cluster_size_ = parameters["segmentation"]["ec_max_cluster_size"].as<int>();

    // Filter parameters
    leaf_size_ = parameters["filters"]["leaf_size"].as<float>();
    pass_limits_ = parameters["filters"]["pass_limits"].as<std::vector<float>>();
    prism_limits_ = parameters["filters"]["prism_limits"].as<std::vector<float>>();
    min_neighbors_ = parameters["filters"]["outlier_min_neighbors"].as<int>();
    radius_search_ = parameters["filters"]["outlier_radius_search"].as<float>();

    point_cloud_sub_ = nh_.subscribe(point_cloud_topic_, 10, &PointCloudProc::pointCloudCb, this);


    if (debug_) {
      plane_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("plane_cloud", 10);
      debug_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("debug_cloud", 10);
      tabletop_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("tabletop_cloud", 10);
    }
}


void PointCloudProc::pointCloudCb(const sensor_msgs::PointCloud2ConstPtr &msg) {
    boost::mutex::scoped_lock lock(pc_mutex_);
    cloud_raw_ros_ = *msg;
}



bool PointCloudProc::transformPointCloud() {
    boost::mutex::scoped_lock lock(pc_mutex_);

    cloud_transformed_->clear();

    tf::TransformListener listener;
    std::string target_frame = cloud_raw_ros_.header.frame_id; //

    listener.waitForTransform(fixed_frame_, target_frame, ros::Time(0), ros::Duration(2.0));
    tf::StampedTransform transform;
    tf::Transform cloud_transform;

    try{
      listener.lookupTransform(fixed_frame_, target_frame, ros::Time(0), transform);
      cloud_transform.setOrigin(transform.getOrigin());
      cloud_transform.setRotation(transform.getRotation());

      sensor_msgs::PointCloud2 cloud_transformed;
      pcl_ros::transformPointCloud(fixed_frame_, cloud_transform, cloud_raw_ros_, cloud_transformed);

      pcl::fromROSMsg(cloud_transformed, *cloud_transformed_);

      std::cout << "PCP: point cloud is transformed!" << std::endl;
      return true;

    }
    catch (tf::TransformException ex){
      ROS_ERROR("%s",ex.what());
      return false;
    }


}

bool PointCloudProc::filterPointCloud() {

    // Remove part of the scene to leave table and objects alone
    pass_.setInputCloud (cloud_transformed_);
    pass_.setFilterFieldName ("x");
    pass_.setFilterLimits (pass_limits_[0],  pass_limits_[1]);
    pass_.filter(*cloud_filtered_);
    pass_.setInputCloud (cloud_filtered_);
    pass_.setFilterFieldName ("y");
    pass_.setFilterLimits (pass_limits_[2],  pass_limits_[3]);
    pass_.filter(*cloud_filtered_);
    pass_.setInputCloud (cloud_filtered_);
    pass_.setFilterFieldName ("z");
    pass_.setFilterLimits (pass_limits_[4],  pass_limits_[5]);
    pass_.filter(*cloud_filtered_);

    std::cout << "PCP: point cloud is filtered!" << std::endl;
    if (cloud_filtered_->points.size() == 0) {
        std::cout <<  "PCP: point cloud is empty after filtering!" << std::endl;
        return false;
    }

  // Downsample point cloud
//   vg_.setInputCloud (cloud_filtered_);
//   vg_.setLeafSize (leaf_size_, leaf_size_, leaf_size_);
//   vg_.filter (*cloud_filtered_);

    return true;
}

bool PointCloudProc::removeOutliers(CloudT::Ptr in, CloudT::Ptr out) {

    outrem_.setInputCloud(in);
    outrem_.setRadiusSearch(radius_search_);
    outrem_.setMinNeighborsInRadius(min_neighbors_);
    outrem_.filter (*out);

}

bool PointCloudProc::segmentSinglePlane(point_cloud_proc::Plane& plane, char axis) {
//    boost::mutex::scoped_lock lock(pc_mutex_);
    std::cout << "PCP: segmenting single plane..." << std::endl;

    if(!transformPointCloud()){
      std::cout << "PCP: couldn't transform point cloud!" << std::endl;
      return false;
    }

    if(!filterPointCloud()){
      std::cout << "PCP: couldn't filter point cloud!" << std::endl;
      return false;
    }



    CloudT::Ptr cloud_plane (new CloudT);
    pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients);
    pcl::PointIndices::Ptr inliers (new pcl::PointIndices);

    Eigen::Vector3f axis_vector = Eigen::Vector3f(0.0, 0.0, 0.0);

    if(axis == 'x'){
      axis_vector[0] = 1.0;
    }
    else if(axis == 'y'){
      axis_vector[1] = 1.0;
    }
    else if(axis == 'z'){
      axis_vector[2] = 1.0;
    }

    seg_.setOptimizeCoefficients (true);
    seg_.setMaxIterations(max_iter_);
    seg_.setModelType (pcl::SACMODEL_PERPENDICULAR_PLANE);
    seg_.setMethodType (pcl::SAC_RANSAC);
    seg_.setAxis(axis_vector);
    seg_.setEpsAngle(eps_angle_ * (M_PI/180.0f));
    seg_.setDistanceThreshold (single_dist_thresh_);
    seg_.setInputCloud (cloud_filtered_);
    seg_.segment (*inliers, *coefficients);


    if (inliers->indices.size() == 0){
      std::cout << "PCP: plane is empty!" << std::endl;
      return false;
    }

    extract_.setInputCloud (cloud_filtered_);
    extract_.setNegative(false);
    extract_.setIndices (inliers);
    extract_.filter (*cloud_plane);

    if (debug_) {
      std::cout << "PCP: # of points in plane: " << cloud_plane->points.size() << std::endl;
      plane_cloud_pub_.publish(cloud_plane);
    }

    cloud_hull_->clear();
    chull_.setInputCloud (cloud_plane);
    chull_.setDimension(2);
    chull_.reconstruct (*cloud_hull_);

    // Get cloud
    pcl::toROSMsg(*cloud_plane, plane.cloud);

    // Construct plane object msg
    pcl_conversions::fromPCL(cloud_plane->header, plane.header);

    // Get plane center
    Eigen::Vector4f center;
    pcl::compute3DCentroid(*cloud_plane, center);
    plane.center.x = center[0];
    plane.center.y = center[1];
    plane.center.z = center[2];

    // Get plane min and max values
    Eigen::Vector4f min_vals, max_vals;
    pcl::getMinMax3D(*cloud_plane, min_vals, max_vals);

    plane.min.x = min_vals[0];
    plane.min.y = min_vals[1];
    plane.min.z = min_vals[2];


    plane.max.x = max_vals[0];
    plane.max.y = max_vals[1];
    plane.max.z = max_vals[2];

    // Get plane polygon
    for (int i = 0; i < cloud_hull_->points.size (); i++) {
        geometry_msgs::Point32 p;
        p.x = cloud_hull_->points[i].x;
        p.y = cloud_hull_->points[i].y;
        p.z = cloud_hull_->points[i].z;

        plane.polygon.push_back(p);
    }

    // Get plane coefficients
    plane.coef[0] = coefficients->values[0];
    plane.coef[1] = coefficients->values[1];
    plane.coef[2] = coefficients->values[2];
    plane.coef[3] = coefficients->values[3];

    plane.size.data = cloud_plane->points.size();

//    extract_.setNegative(true);
//    extract_.filter(*cloud_filtered_);

    return true;
}

bool PointCloudProc::segmentMultiplePlane(std::vector<point_cloud_proc::Plane>& planes) {

//    boost::mutex::scoped_lock lock(pc_mutex_);

    if(!transformPointCloud()){
      std::cout << "PCP: couldn't transform point cloud!" << std::endl;
      return false;
    }

    if(!filterPointCloud()){
      std::cout << "PCP: couldn't filter point cloud!" << std::endl;
      return false;
    }

    CloudT plane_clouds;
    plane_clouds.header.frame_id = cloud_transformed_->header.frame_id;
    point_cloud_proc::Plane plane_object_msg;

    int no_planes = 1;
    CloudT::Ptr cloud_plane_raw (new CloudT);
    CloudT::Ptr cloud_plane (new CloudT);
    CloudT::Ptr cloud_hull (new CloudT);

//    Eigen::Vector3f axis = Eigen::Vector3f(0.0,0.0,1.0); //z axis
//    seg_.setModelType (pcl::SACMODEL_PERPENDICULAR_PLANE);
//    seg_.setAxis(axis);

    seg_.setOptimizeCoefficients (true);
    seg_.setModelType(pcl::SACMODEL_PLANE);
    seg_.setMaxIterations(max_iter_);
    seg_.setMethodType (pcl::SAC_RANSAC);
    seg_.setEpsAngle(eps_angle_ * (M_PI/180.0f));
    seg_.setDistanceThreshold(multi_dist_thresh_);

    while(true) {

        pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients);
        pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
        seg_.setInputCloud (cloud_filtered_);
        seg_.segment (*inliers, *coefficients);

        if (inliers->indices.size() == 0 and no_planes == 0) {
            std::cout <<  "PCP: no plane found!!!" << std::endl;
            return false;
        }

        else if (inliers->indices.size() < min_plane_size_) {
            break;
        }


//            std::cout << "PCP: plane coefficients : " << coefficients->values[0]  << " "
//                                                      << coefficients->values[1]  << " "
//                                                      << coefficients->values[2]  << " "
//                                                      << coefficients->values[3]  << std::endl;


        extract_.setInputCloud (cloud_filtered_);
        extract_.setNegative(false);
        extract_.setIndices (inliers);
        extract_.filter (*cloud_plane);

//        plane_proj_.setInputCloud(cloud_plane_raw);
//        plane_proj_.setModelType(pcl::SACMODEL_PLANE);
//        plane_proj_.setModelCoefficients (coefficients);
//        plane_proj_.filter (*cloud_plane);


//        removeOutliers(cloud_plane_raw, cloud_plane);

        plane_clouds +=*cloud_plane;

        chull_.setInputCloud (cloud_plane);
        chull_.setDimension(2);
        chull_.reconstruct (*cloud_hull);

        Eigen::Vector4f center;
        pcl::compute3DCentroid(*cloud_hull, center); // TODO: Compare with cloud_plane center

        Eigen::Vector4f min_vals, max_vals;
        pcl::getMinMax3D(*cloud_plane, min_vals, max_vals);

        // Get cloud
        pcl::toROSMsg(*cloud_plane, plane_object_msg.cloud);

        // Construct plane object msg
        pcl_conversions::fromPCL(cloud_plane->header, plane_object_msg.header);

        // Get plane center
        plane_object_msg.center.x = center[0];
        plane_object_msg.center.y = center[1];
        plane_object_msg.center.z = center[2];

        // Get plane min and max values
        plane_object_msg.min.x = min_vals[0];
        plane_object_msg.min.y = min_vals[1];
        plane_object_msg.min.z = min_vals[2];


        plane_object_msg.max.x = max_vals[0];
        plane_object_msg.max.y = max_vals[1];
        plane_object_msg.max.z = max_vals[2];

        // Get plane polygon
        for (int i = 0; i < cloud_hull->points.size (); i++) {
            geometry_msgs::Point32 p;
            p.x = cloud_hull->points[i].x;
            p.y = cloud_hull->points[i].y;
            p.z = cloud_hull->points[i].z;
            plane_object_msg.polygon.push_back(p);
        }

        // Get plane coefficients
        plane_object_msg.coef[0] = coefficients->values[0];
        plane_object_msg.coef[1] = coefficients->values[1];
        plane_object_msg.coef[2] = coefficients->values[2];
        plane_object_msg.coef[3] = coefficients->values[3];

        std::string axis;

        if(std::abs(coefficients->values[0]) < 1.1 &&
           std::abs(coefficients->values[0]) > 0.9 &&
           std::abs(coefficients->values[1]) < 0.1 &&
           std::abs(coefficients->values[2]) < 0.1){
          plane_object_msg.orientation = point_cloud_proc::Plane::XAXIS;
          axis = "X";
        }

        else if(std::abs(coefficients->values[0]) < 0.1 &&
           std::abs(coefficients->values[1]) > 0.9 &&
           std::abs(coefficients->values[1]) < 1.1 &&
           std::abs(coefficients->values[2]) < 0.1){
          plane_object_msg.orientation = point_cloud_proc::Plane::YAXIS;
          axis = "Y";
        }

        else if(std::abs(coefficients->values[0]) < 0.1 &&
           std::abs(coefficients->values[1]) < 0.1 &&
           std::abs(coefficients->values[2]) < 1.1 &&
           std::abs(coefficients->values[2]) > 0.9){
          plane_object_msg.orientation = point_cloud_proc::Plane::ZAXIS;
          axis = "Z";
        }

        else{
          plane_object_msg.orientation = point_cloud_proc::Plane::NOAXIS;
          axis = "NO";
        }

        std::cout << "PCP: " << no_planes << ". plane segmented! # of points: "
                             << inliers->indices.size() << " axis: " << axis << std::endl;
        no_planes++;

        plane_object_msg.size.data = cloud_plane->points.size();

        planes.push_back(plane_object_msg);
        extract_.setNegative(true);
        extract_.filter(*cloud_filtered_);

        if (debug_) {
          plane_cloud_pub_.publish(cloud_plane);
        }
        ros::Duration(0.2).sleep();
    }




    return true;
}

bool PointCloudProc::extractTabletop() {

    pcl::PointIndices::Ptr tabletop_indices(new pcl::PointIndices);
    prism_.setInputCloud(cloud_filtered_);
    prism_.setInputPlanarHull(cloud_hull_);
    prism_.setHeightLimits(prism_limits_[0], prism_limits_[1]);
    prism_.segment(*tabletop_indices);

    tabletop_indicies_ = tabletop_indices;

    extract_.setInputCloud (cloud_filtered_);
    extract_.setIndices(tabletop_indices);
    extract_.filter(*cloud_tabletop_);

    if (cloud_tabletop_->points.size() == 0) {
        return false;
    }
    else {
        if (debug_) {
          tabletop_pub_.publish(cloud_tabletop_);
        }
        return true;
    }
}

bool PointCloudProc::clusterObjects(std::vector<point_cloud_proc::Object>& objects) {

    if (!extractTabletop()){
      return false;
    }

    pcl::search::KdTree<pcl::PointXYZRGB>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZRGB>);

    tree->setInputCloud (cloud_tabletop_);
    std::vector<pcl::PointIndices> cluster_indices;

    ec_.setClusterTolerance (cluster_tol_);
    ec_.setMinClusterSize (min_cluster_size_);
    ec_.setMaxClusterSize (max_cluster_size_);
    ec_.setSearchMethod (tree);
    ec_.setInputCloud (cloud_tabletop_);
    ec_.extract (cluster_indices);

    pcl::PCA<PointT> pca_ = new pcl::PCA<PointT>;
    pcl::NormalEstimationOMP<PointT, PointNT> ne(4);

    int k = 0;
    if (cluster_indices.size() == 0)
        return false;
    else
      std::cout << "PCP: number of objects: " << cluster_indices.size() << std::endl;

    for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin (); it != cluster_indices.end (); ++it){

        CloudT::Ptr cluster (new CloudT);
        CloudNT::Ptr cluster_normals (new CloudNT);
        for (std::vector<int>::const_iterator pit = it->indices.begin (); pit != it->indices.end (); ++pit){
          cluster->points.push_back (cloud_tabletop_->points[*pit]);
        }

        pcl::PointIndices::Ptr object_indices(new pcl::PointIndices);
        object_indices->indices = it->indices;
        Eigen::Matrix3f eigen_vectors;
        Eigen::Vector3f eigen_values;
        pca_.setInputCloud(cloud_tabletop_);
        pca_.setIndices(object_indices);
        eigen_vectors = pca_.getEigenVectors();
        eigen_values = pca_.getEigenValues();

        cluster->header = cloud_tabletop_->header;
        cluster->width = cluster->points.size();
        cluster->height = 1;
        cluster->is_dense = true;

        // Compute point normals
        pcl::search::KdTree<PointT>::Ptr tree (new pcl::search::KdTree<PointT> ());
        ne.setInputCloud(cluster);
        ne.setSearchMethod (tree);
        ne.setKSearch(k_search_);
        ne.compute (*cluster_normals);

        point_cloud_proc::Object object;

        // Get object point cloud
        pcl_conversions::fromPCL(cluster->header, object.header);

        // Get point normals
        for (int i = 0; i < cluster_normals->points.size(); i++) {
            geometry_msgs::Vector3 normal;
            normal.x = cluster_normals->points[i].normal_x;
            normal.y = cluster_normals->points[i].normal_y;
            normal.z = cluster_normals->points[i].normal_z;
            object.normals.push_back(normal);
        }

        // Get cloud
        pcl::toROSMsg(*cluster, object.cloud);

        // Get object center
        Eigen::Vector4f center;
        pcl::compute3DCentroid(*cluster, center);
        object.center.x = center[0];
        object.center.y = center[1];
        object.center.z = center[2];

        // geometry_msgs::Pose cluster_pose;
        object.pose.position.x = center[0];
        object.pose.position.y = center[1];
        object.pose.position.z = center[2];
        Eigen::Quaternionf quat (eigen_vectors);
        quat.normalize();

        object.pose.orientation.x = quat.x();
        object.pose.orientation.y = quat.y();
        object.pose.orientation.z = quat.z();
        object.pose.orientation.w = quat.w();

        // Get min max points coords
        Eigen::Vector4f min_vals, max_vals;
        pcl::getMinMax3D(*cluster, min_vals, max_vals);

        object.min.x = min_vals[0];
        object.min.y = min_vals[1];
        object.min.z = min_vals[2];
        object.max.x = max_vals[0];
        object.max.y = max_vals[1];
        object.max.z = max_vals[2];

        k++;
        if(debug_){
          std::cout << "PCP: # of points in object " << k << " : " << cluster->points.size() << std::endl;
        }


        objects.push_back(object);
    }
    return true;
}

bool PointCloudProc::get3DPoint(int col, int row, geometry_msgs::PointStamped &point) {

  if(!transformPointCloud()){
    std::cout << "PCP: couldn't transform point cloud!" << std::endl;
    return false;
  }

  pcl_conversions::fromPCL(cloud_transformed_->header, point.header);

  if (pcl::isFinite(cloud_transformed_->at(col, row))) {
    point.point.x = cloud_transformed_->at(col, row).x;
    point.point.y = cloud_transformed_->at(col, row).y;
    point.point.z = cloud_transformed_->at(col, row).z;
    return true;
  }else{
    std::cout << "PCP: The 3D point is not valid!" << std::endl;
    return false;
  }

}

bool PointCloudProc::getObjectFromBBox(int *bbox, point_cloud_proc::Object& object) {

  if(!transformPointCloud()){
    std::cout << "PCP: couldn't transform point cloud!" << std::endl;
    return false;
  }
  sensor_msgs::PointCloud2 object_cloud_ros;
  pcl_conversions::fromPCL(cloud_transformed_->header, object.header);

  CloudT::Ptr object_cloud(new CloudT);
  CloudT::Ptr object_cloud_filtered(new CloudT);
  object_cloud->header = cloud_transformed_->header;

  for (int i = bbox[0]; i < bbox[2]; i++){
    for (int j = bbox[1]; j < bbox[3]; j++){
      if (pcl::isFinite(cloud_transformed_->at(i, j))) {
        object_cloud->push_back(cloud_transformed_->at(i, j));
      }
    }

  }

  removeOutliers(object_cloud, object_cloud_filtered);
  if(object_cloud_filtered->empty()){
    std::cout << "PCP: object cloud is empty after removing outliers!" << std::endl;
    return false;
  }

  Eigen::Vector4f min_vals, max_vals;

  pcl::getMinMax3D(*object_cloud_filtered, min_vals, max_vals);

  object.min.x = min_vals[0];
  object.min.y = min_vals[1];
  object.min.z = min_vals[2];
  object.max.x = max_vals[0];
  object.max.y = max_vals[1];
  object.max.z = max_vals[2];

  Eigen::Vector4f center;
  pcl::compute3DCentroid(*object_cloud_filtered, center);
  object.center.x = center[0];
  object.center.y = center[1];
  object.center.z = center[2];

  debug_cloud_pub_.publish(object_cloud_filtered);
  return true;

}

bool PointCloudProc::trianglePointCloud(sensor_msgs::PointCloud2& cloud, pcl_msgs::PolygonMesh& mesh) {

  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_xyz (new pcl::PointCloud<pcl::PointXYZ>);
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_xyz_ (new pcl::PointCloud<pcl::PointXYZ>);
//  pcl::copyPointCloud(*cloud, *cloud_xyz);
  pcl::fromROSMsg(cloud, *cloud_xyz_);
  pcl::VoxelGrid<pcl::PointXYZ> vg;
  vg.setInputCloud (cloud_xyz_);
//  vg.setLeafSize (0.05f, 0.05f, 0.05f);
  vg.setLeafSize(0.005f, 0.005f, 0.005f);
  vg.filter(*cloud_xyz);

  // Compute point normals
  pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> ne;
  pcl::PointCloud<pcl::Normal>::Ptr normals (new pcl::PointCloud<pcl::Normal>);
  pcl::PointCloud<pcl::PointNormal>::Ptr cloud_normals (new pcl::PointCloud<pcl::PointNormal>);
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZ> ());

  tree->setInputCloud(cloud_xyz);
  ne.setInputCloud(cloud_xyz);
  ne.setSearchMethod (tree);
  ne.setKSearch(20);
  ne.compute (*normals);

  pcl::concatenateFields (*cloud_xyz, *normals, *cloud_normals);

  pcl::search::KdTree<pcl::PointNormal>::Ptr tree2 (new pcl::search::KdTree<pcl::PointNormal>);
  tree2->setInputCloud(cloud_normals);

//  pcl::PolygonMesh triangles;
  pcl::PolygonMesh::Ptr triangles(new pcl::PolygonMesh());
  gp3_.setSearchRadius (0.2);
  gp3_.setMu (2.5);
  gp3_.setMaximumNearestNeighbors (100);
  gp3_.setMaximumSurfaceAngle(M_PI/4); // 45 degrees
  gp3_.setMinimumAngle(M_PI/18); // 10 degrees
  gp3_.setMaximumAngle(2*M_PI/3); // 120 degrees
  gp3_.setNormalConsistency(false);

  gp3_.setInputCloud (cloud_normals);
  gp3_.setSearchMethod (tree2);
  gp3_.reconstruct (*triangles);


  pcl_conversions::fromPCL(*triangles, mesh);


//  pcl::PointCloud<pcl::PointXYZ> triangle_cloud;
//  pcl::fromPCLPointCloud2(triangles.cloud, triangle_cloud);
//  int i = 0;
//  mesh.vertices.resize(t)
//  for(auto point : triangle_cloud.points){
//    geometry_msgs::Point p;
//    p.x = point.x;
//    p.y = point.y;
//    p.z = point.z;
//
//    mesh.vertices.push_back(p);
//    triangles.polygons[i]
//  }


  return true;
}

void PointCloudProc::getRemainingCloud(sensor_msgs::PointCloud2& cloud) {
//  sensor_msgs::PointCloud2::Ptr cloud;
  pcl::toROSMsg(*cloud_filtered_, cloud);

//  return cloud;
}

sensor_msgs::PointCloud2::Ptr PointCloudProc::getTabletopCloud() {
  sensor_msgs::PointCloud2::Ptr cloud;
  pcl::toROSMsg(*cloud_tabletop_, *cloud);

  return cloud;
}

PointCloudProc::CloudT::Ptr PointCloudProc::getFilteredCloud() {
//  sensor_msgs::PointCloud2::Ptr filtered_cloud;
//  pcl::toROSMsg(*cloud_filtered_, *filtered_cloud);

  return cloud_filtered_;
}

pcl::PointIndices::Ptr PointCloudProc::getTabletopIndicies() {
  return tabletop_indicies_;
}