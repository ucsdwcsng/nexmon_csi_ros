 #include <ros/ros.h>
#include "shutils.h"
#include "utils.h"
#include <vector>
#include <stdio.h>
#include <regex>
#include <sstream>
#include <iomanip>
#include <rf_msgs/AccessPoints.h>

const std::regex channel_ex("Channel:(\\d+)");
const std::regex rssi_ex("Signal level=([\\-0-9]+) dBm");

ros::Publisher pub_5;
ros::Publisher pub_2;

int main(int argc, char* argv[]){
  ros::init(argc, argv, "ap_scanner", ros::init_options::NoSigintHandler);
  ros::NodeHandle nh("~");
  std::string iface;
  std::string topic;
  double scan_period;
  mac_filter filter;
  std::string mac_filter_init;
  nh.param<std::string>("iface", iface);
  nh.param<std::string>("topic", topic, "aps");
  nh.param<std::string>("mac_filter", mac_filter_init, "*:*:*:*:*:*");
  nh.param<double>("period", scan_period, 20.0);

  filter = mac_filter_str(mac_filter_init);
  
  char topic2[64];
  sprintf(topic2,"%s_2",topic.c_str());
  char topic5[64];
  sprintf(topic5,"%s_5",topic.c_str());
  
  pub_5 = nh.advertise<rf_msgs::AccessPoints>(topic5,3);
  pub_2 = nh.advertise<rf_msgs::AccessPoints>(topic2,3);

  char scan_cmd[128];
  sprintf(scan_cmd,"sudo iwlist %s scanning | egrep --color=never 'Address|Signal level|Channel:' 2>&1",iface.c_str());
  
  while(ros::ok()){
	std::string s = sh_exec_block(scan_cmd);
	rf_msgs::AccessPoints aps_5;
	rf_msgs::AccessPoints aps_2;
	ROS_INFO("Detected APs:");
	size_t s_start = s.find("Cell",0);
	size_t s_end = s_start;
	while((s_end=s.find("Cell", s_start+1)) != std::string::npos){
	  std::string entry = s.substr(s_start, s_end-s_start);
	  std::smatch mac_match;
	  std::smatch chan_match;
	  std::smatch rssi_match;
	  
	  rf_msgs::Station ap;
	  bool pass_filt = true;
	  if(std::regex_search(entry,mac_match,addr_ex)){
		for(int i = 1; i < mac_match.size(); ++i){
		  ap.mac[i-1] = std::strtol(mac_match[i].str().c_str(), NULL, 16);
		}
	  }
	  else pass_filt = false;

	  if(!mac_cmp(ap.mac.data(), filter)) pass_filt = false;
	  
	  if(std::regex_search(entry,chan_match,channel_ex)){
		ap.channel = std::stoi(chan_match[1].str());
	  }
	  else pass_filt = false;
	  
	  if(std::regex_search(entry,rssi_match, rssi_ex)){
		ap.rssi = std::stoi(rssi_match[1].str());
	  }
	  else pass_filt = false;

	  s_start = s_end;
	  if(!pass_filt) continue;
	  
	  ROS_INFO("%s:\tchan %d\trssi %d",hr_mac(ap.mac.data()).c_str(),ap.channel,ap.rssi);
	  
	  if(ap.channel <= 14){
		aps_2.aps.push_back(ap);
	  }
	  else{
		aps_5.aps.push_back(ap);
	  }
	  
	  s_start = s_end;
	}
	std::sort(aps_5.aps.begin(), aps_5.aps.end(), [](rf_msgs::Station a1, rf_msgs::Station a2){
											return a1.rssi > a2.rssi;
										  }
	  );
	
	pub_5.publish(aps_5);
	pub_2.publish(aps_2);

    ros::Duration(scan_period).sleep();
    ROS_INFO("time is %f", ros::Time::now().toSec());
  }
}

