//reads udp data from nexmon_csi running on an ASUS
//converts to CSI message format
#include "nexcsiserver.h"

//CSI-Buffering related globals
std::vector<csi_instance> channel_current;
uint16_t last_seq;

//Buffer in which the full CSI message is reconstructed
double* csi_r_out = NULL;
double* csi_i_out = NULL;
//size of the above buffers
size_t csi_size = 0;

//holds the remote client's ssh process so we can shut it down properly
FILE* cli_fp = NULL;
//same for the TX process
FILE* beacon_process_fp = NULL;

//Info about the ASUS that the node is connected to
std::string cfg_sta_ip;
std::string cfg_sta_pass;
std::string cfg_sta_user;

//Forward the packets over tcpdump->netcat (older kernels won't receive the udp broadcasts)
bool cfg_use_tcp_transport = false;

//Don't configure
bool cfg_do_not_start_nexmon = false;

//Info about the node itself
std::string g_this_device_user;
std::string g_this_device_IP;

//publisher
ros::Publisher pub_csi;
ros::Subscriber sub_ap;

//info about current wireless settings
int cfg_channel, cfg_bw;
double cfg_beacon_period;

//parameters that differ between 2.4GHz and 5GHz.
std::string cfg_sta_bcm_iface;
int cfg_sta_num_tx_cores;

//MAC addresses to be filtered for in software
mac_filter filter;
bool use_software_mac_filter = true;

//if not "", the node will listen on the given topic for a list of APs and try to collect CSI from
//the first AP on the list.
std::string cfg_ap_scanner_topic;

//various buffers
unsigned char *csi_buf, *csi_data;

int main(int argc, char* argv[]){
  //setup ros
  ros::init(argc, argv, "nexcsi", ros::init_options::NoSigintHandler);
  ros::NodeHandle nh("~");

  ros::AsyncSpinner spinner(0);
  spinner.start();

  ros::ServiceServer set_chanspec_srv = nh.advertiseService<wiros_csi_node::ConfigureCSI::Request, wiros_csi_node::ConfigureCSI::Response>("configure_csi",config_csi_callback);

  //handle shutdown
  signal(SIGINT, handle_shutdown);

  //read params
  setup_params(nh);

  //subscribe to the AP scanner topic if it's set.
  if(cfg_ap_scanner_topic != std::string("")){
    sub_ap = nh.subscribe(cfg_ap_scanner_topic, 10, ap_info_callback);
    ROS_INFO("Subscribing: %s", sub_ap.getTopic().c_str());
  }

  //display starting chanspec
  ROS_INFO("chanspec %d/%d", (int)cfg_channel, (int)cfg_bw);

  //Resolve the ASUS's IP, and ensure that it is pingable.
  //This also discovers our own IP and the interface we
  //will use to talk to the AP.
  setup_asus_connection();

  if(!ros::ok()){
    return 0;
  }

  //Start nexmon on the ASUS.
  ROS_INFO("Configuring Receiver...");

  if(cfg_do_not_start_nexmon){ROS_WARN("Not starting nexmon.");}
  else{
    bool unconfigured = true;
    while(unconfigured){
      if(set_chanspec((int)cfg_channel, (int)cfg_bw)){
	ROS_ERROR("Invalid channel or bandwidth.");
	exit(1);
      }
      std::string res_out = reset_nexmon();
      ROS_INFO("\n***\nSetup Output:\n\n%s\n***", res_out.c_str());
      if(res_out.find("Permission denied") != std::string::npos){
	ROS_ERROR("A device was found at %s, but it refused SSH access.\nPlease check the 'asus_pwd' param and ensure it is set to the device's password.\nCurrent passsword: %s\nThis may also be caused by setup scripts not having the correct permissions set.",cfg_sta_ip.c_str(),cfg_sta_pass.c_str());
	exit(EXIT_FAILURE);
      }
      if(res_out.find("Connection refused") != std::string::npos){
	ROS_INFO("Waiting 5 seconds and retrying...");
	sleep(5);
      }
      else{
	unconfigured = false;
      }
    }
  }

  //Start the beacon process. This sets g_beacon_process_fp.
  if(cfg_beacon_period > 0) {
    setup_beacon();
  }

  //Advertise csi topic.
  char topic_name[256];
  sprintf(topic_name, "/csi");
  pub_csi = nh.advertise<rf_msgs::Wifi>(topic_name,10);
  ROS_INFO("Publishing: %s", pub_csi.getTopic().c_str());


  int sockfd, connfd;
  socklen_t len;

  struct sockaddr_in servaddr, cliaddr;
  socklen_t sockaddr_len = sizeof(cliaddr);

  // Create socket
  if (cfg_use_tcp_transport) {//forward over tcpdump-netcat
	if ( (sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) {
	  perror("socket creation failed");
	  exit(EXIT_FAILURE);
	}
  }
  else{//forward over udp
    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0 ) {
      perror("socket creation failed");
      exit(EXIT_FAILURE);
    }
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 10000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
      perror("Error");
    }
  }
  memset(&servaddr, 0, sizeof(servaddr));
  memset(&cliaddr, 0, sizeof(cliaddr));

  // Filling server information
  servaddr.sin_family    = AF_INET; // IPv4
  servaddr.sin_addr.s_addr = INADDR_ANY;
  if(cfg_use_tcp_transport)
    servaddr.sin_port = htons(PORT_TCP);
  else
    servaddr.sin_port = htons(PORT);

  //can't do &(1) in c++ so need to do this
  int yes=1;
  setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(int));
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(int));
  // Bind the socket with the server address
  if ( bind(sockfd, (const struct sockaddr *)&servaddr,
	    sizeof(servaddr)) < 0 )
    {
      ROS_INFO("bind failed: %s", strerror(errno));
      exit(EXIT_FAILURE);
    }

  ROS_INFO("Opened socket.");

  int n;

  if(cfg_use_tcp_transport){
    setup_tcpdump(g_this_device_IP);
    while(ros::ok() && (listen(sockfd, 5)) != 0){
      ROS_INFO("Waiting for TCP connection...");
      sleep(1);
    }
    while(ros::ok()) {
      connfd = accept(sockfd, (SA * ) & cliaddr, &len);
	  
	  
      if (connfd == -1 && errno == EAGAIN) {
	sleep(0.05);
	continue;
      }
      else if(connfd > 0) {
	ROS_INFO("Accepted Connection from %s", cfg_sta_ip.c_str());
	break;
      }
      else{
	ROS_ERROR("Connection failed.");
	exit(1);
      }
    }
  }

  ROS_INFO("Starting CSI collection");
  //buffer to hold incoming UDP data
  char buffer[MAXLINE];

  //incoming data is separated by packet, stored in here
  csi_buf = new unsigned char[CSI_BUF_SIZE];
  csi_data = new unsigned char[CSI_BUF_SIZE];
  //CSI binary data is then processed into a csi_data struct, put in here
  std::queue<csi_instance> pkts;
  int it = 0;
  size_t csi_pos = 0;

  ROS_WARN("Filtering for MAC Addresses: %s",hr_mac_filt(filter).c_str());

  //normal udp broadcast version
  if(!cfg_use_tcp_transport){
    while(ros::ok() && !ros::isShuttingDown()){
      if ((n = recvfrom(sockfd, csi_buf, CSI_BUF_SIZE, 0, (struct sockaddr *)&cliaddr, &sockaddr_len)) == -1){
	if(errno == ETIMEDOUT || errno == EAGAIN){
	  continue;
	}
	ROS_ERROR("Socket Error: %s", strerror(errno));
      }
      if(n > 0){
	parse_csi(csi_buf, n);
      }
    }
  }

  else{//decode the received packet from tcpdump
    while(ros::ok()){

      n = read(connfd, buffer, MAXLINE);
      if(n == 0) continue;

      //if too much data is accumulating with no packets found, get rid of all our data
      if(csi_pos + n > CSI_BUF_SIZE){
	ROS_ERROR("CSI BUFFER OVERFLOWED, FLUSHING");
	memset(csi_buf, 0, CSI_BUF_SIZE);
	csi_pos = 0;
      }

      //add new data to buffer
      memcpy(csi_buf + csi_pos, buffer, n);
      csi_pos += n;
      size_t hdr_pos = 0;
      //searches for start of the next packet from first 8 bytes (as we expect the beginning of the buffer to be the header of the packet currently being received)
      while((hdr_pos = find_csi_hdr(csi_buf)) != NO_HDR_IN_BUF){
	memcpy(csi_data, csi_buf, hdr_pos);
	//size of remaining data in buffer
	csi_pos = csi_pos - hdr_pos;
	//shift data back in buffer
	memcpy(csi_buf, csi_buf + hdr_pos, csi_pos);
	//now process csi data
	parse_csi(csi_data+8+8, hdr_pos);
	//clear empty space in the buffer
	memset(csi_buf + csi_pos, 0, CSI_BUF_SIZE - csi_pos);
      }
    }
  }
}

void parse_csi(unsigned char* data, size_t nbytes){
  //8 is sizeof the ethernet header
  csi_udp_frame *rxframe = reinterpret_cast<csi_udp_frame*>(data);
  if(use_software_mac_filter){
    if(!mac_cmp(rxframe->src_mac, filter)) return;
  }
  csi_instance out;

  out.rssi = rxframe->rssi;
  //ROS_INFO("%.4hhx", rxframe->kk1);
  memcpy(out.source_mac, rxframe->src_mac, 6);
  out.seq = rxframe->seqCnt;
  out.fc = (uint8_t)(rxframe->fc);

  out.tx = (rxframe->csiconf >> 11) & 0x3;
  out.rx = (rxframe->csiconf >> 8) & 0x3;
  out.bw = (rxframe->chanspec>>11) & 0x07;
  out.channel = (rxframe->chanspec) & 255;

  switch(out.bw)
    {
    case 0x4:
      out.bw = 80;
      break;
    case 0x3:
      out.bw = 40;
      break;
    case 0x2:
      out.bw = 20;
      break;
    default:
      ROS_ERROR("Invalid Bandwidth received %d", out.bw);
      return;
    }

  uint32_t n_sub = (uint32_t)(((float)out.bw) *3.2);
  size_t csi_nbytes = (size_t)(n_sub * sizeof(int32_t));

  uint32_t *csi = reinterpret_cast<uint32_t*>(data+sizeof(csi_udp_frame));

  out.n_sub = n_sub;
  out.csi_r = new double[n_sub];
  out.csi_i = new double[n_sub];

  //decode CSI
  uint64_t c_r, c_i;
  uint64_t c_r_buf[n_sub], c_i_buf[n_sub];
  for(int i = 0; i < n_sub; ++i){

    uint32_t c = (uint64_t)csi[i];
    c_r=0;
    c_i=0;
    uint32_t exp = ((int32_t)(c & e_mask) - 31 + 1023);
    uint32_t r_exp = exp;
    uint32_t i_exp = exp;

    uint32_t r_mant = (c&r_mant_mask) >> 18;
    uint32_t i_mant = (c&i_mant_mask) >> 6;

    //construct real mantissa
    uint32_t e_shift = 0;
    while(!(r_mant & count_mask)){
      r_mant *= 2;
      e_shift += 1;
      if(e_shift == 10){
	r_exp = 1023;
	e_shift = 0;
	r_mant = 0;
	break;
      }
    }
    r_exp -= e_shift;

    //construct imaginary mantissa
    e_shift = 0;
    while(!(i_mant & count_mask)){
      i_mant *= 2;
      e_shift += 1;
      if(e_shift == 10){
	i_exp = 1023;
	e_shift = 0;
	i_mant = 0;
	break;
      }
    }
    i_exp -= e_shift;

    //construct doubles
    c_r |= (uint64_t)(c & r_sign_mask) << 34;
    c_i |= (uint64_t)(c & i_sign_mask) << 46;

    c_r |= ((uint64_t)(r_mant & mant_mask)) << 42;
    c_i |= ((uint64_t)(i_mant & mant_mask)) << 42;

    c_r |= ((uint64_t)r_exp)<<52;
    c_i |= ((uint64_t)i_exp)<<52;

    //place doubles
    c_r_buf[i] = c_r;
    c_i_buf[i] = c_i;

  }

  //copy to struct
  memcpy(out.csi_r, c_r_buf, sizeof(double)*n_sub);
  memcpy(out.csi_i, c_i_buf, sizeof(double)*n_sub);

  //scan for repeated tx-rx
  bool new_csi = false;
  for(auto ch_it = channel_current.begin(); ch_it != channel_current.end(); ++ch_it){
    int out_comb = out.tx*4 + out.rx;
    if(ch_it->tx*4 + ch_it->rx == out_comb){
      new_csi = true;
    }
  }
  if(out.seq != last_seq && channel_current.size() > 0){
    new_csi = true;
  }

  if(new_csi){
    publish_csi(channel_current);
    channel_current.clear();
  }

  last_seq = out.seq;
  //save the currently extracted CSI
  channel_current.push_back(out);
}

void publish_csi(std::vector<csi_instance> &channel_current){
  //4x4 matrices, with n_sub elements each, w/ interleaved 4 byte real + imag parts
  csi_instance csi_0 = channel_current.at(0);
  size_t rx_stride = csi_0.n_sub;
  size_t rx2 = rx_stride / 2;
  size_t tx_stride = rx_stride*4;
  size_t num_floats = tx_stride*4;
  rf_msgs::Wifi msgout;
  if(num_floats > csi_size){
    delete csi_r_out;
    delete csi_i_out;
    //statically allocate csi_out for now
    csi_r_out = new double[num_floats];
    csi_i_out = new double[num_floats];
    csi_size = num_floats;
  }
  msgout.header.stamp = ros::Time::now();
  msgout.ap_id = 0;
  msgout.txmac = std::vector<unsigned char>(csi_0.source_mac, csi_0.source_mac + 6);
  msgout.chan = csi_0.channel;
  msgout.n_sub = csi_0.n_sub;
  msgout.seq_num = csi_0.seq;
  msgout.fc = csi_0.fc;
  msgout.n_rows = 4;
  msgout.n_cols = 4;
  msgout.bw = csi_0.bw;
  msgout.mcs = 0;
  msgout.rssi = (int32_t)(csi_0.rssi);
  ROS_INFO("%s:RSSI%d/seq%d/fc%.2hhx/chan%d/rx%s",hr_mac(csi_0.source_mac).c_str(), msgout.rssi, msgout.seq_num, csi_0.fc, msgout.chan, cfg_sta_ip.c_str());
  memset(csi_r_out,0,num_floats*sizeof(double));
  memset(csi_i_out,0,num_floats*sizeof(double));
  msgout.rx_id = cfg_sta_ip;
  msgout.msg_id = 0;
  for(auto c = channel_current.begin(); c != channel_current.end(); ++c){
    size_t csi_idx = rx_stride*c->rx + tx_stride*c->tx;
    //fft-shift the data
    for(int bin = 0; bin < rx2; ++bin){
      csi_r_out[csi_idx + bin + rx2] = (c->csi_r[bin]);
      csi_i_out[csi_idx + bin + rx2] = (c->csi_i[bin]);
      csi_r_out[csi_idx + bin] = (c->csi_r[bin + rx2]);
      csi_i_out[csi_idx + bin] = (c->csi_i[bin + rx2]);
    }
  }
  msgout.csi_real = std::vector<double>(csi_r_out, csi_r_out + num_floats);
  msgout.csi_imag = std::vector<double>(csi_i_out, csi_i_out + num_floats);
  pub_csi.publish(msgout);
}

void handle_shutdown(int sig){
  ROS_WARN("Shutting down.");
  if(cli_fp){
    ROS_WARN("Closing tcpdump process");
    pclose(cli_fp);
  }
  if(beacon_process_fp){
    ROS_WARN("Closing tx process");
    pclose(beacon_process_fp);
    char killcmd[128];
	
    sprintf(killcmd, "sshpass -p %s ssh -o strictHostKeyChecking=no %s@%s killall send.sh", cfg_sta_pass.c_str(), cfg_sta_user.c_str(), cfg_sta_ip.c_str());
    sh_exec(std::string(killcmd));
  }
  ROS_WARN("Calling ros::shutdown()");
  ros::shutdown();
  ROS_WARN("Done.");

}

//returns true on error
bool set_chanspec(int s_chan, int s_bw){
  if(!(s_bw == -1 || s_bw ==20 || s_bw == 40 || s_bw == 80))
    return true;
  if(s_chan != -1 && s_chan != cfg_channel){
    cfg_channel = s_chan;
    ROS_WARN("Setting CHANNEL to %d", cfg_channel);
  }
  if(s_bw != -1 && s_bw != cfg_bw){
    cfg_bw = s_bw;
    ROS_WARN("Setting BW to %d", cfg_bw);
  }
  return false;
}

bool set_mac_filter(mac_filter filt){
  if(filt.len < 0 || filt.len > 6){
    return true;
  }
  filter = filt;
  ROS_WARN("Set MAC FILTER to %s",hr_mac_filt(filter).c_str());
  if(filt.len == 2){
    use_software_mac_filter = false;
  }
  return false;
}

/**
 * This function SSH-es into the router and configures nexmon to capture the
 * appropriate packets. It uses the cfg_* global parameters.
 */
std::string reset_nexmon(){

  /* Change interface between 2.4 and 5GHz depending on channel */
  if(cfg_channel >= 32){
    cfg_sta_bcm_iface = "eth6";
    cfg_sta_num_tx_cores = cfg_sta_num_tx_cores > 4 ? 4 : cfg_sta_num_tx_cores;
  }
  else{
    cfg_sta_bcm_iface = "eth5";
    cfg_sta_num_tx_cores = cfg_sta_num_tx_cores > 3 ? 3 : cfg_sta_num_tx_cores;
  }
    
  char configcmd[512];
  if(filter.len > 1){
    sprintf(configcmd, "sshpass -p %s ssh -o strictHostKeyChecking=no %s@%s /jffs/csi/setup.sh %d %d 4 %.2hhx:%.2hhx:00:00:00:00 2>&1",
            cfg_sta_pass.c_str(), cfg_sta_user.c_str(), cfg_sta_ip.c_str(), cfg_channel, cfg_bw, filter.mac[0],filter.mac[1]);
  }
  else{
    sprintf(configcmd, "sshpass -p %s ssh -o strictHostKeyChecking=no %s@%s /jffs/csi/setup.sh %d %d 4 2>&1", cfg_sta_pass.c_str(), cfg_sta_user.c_str(), cfg_sta_ip.c_str(), cfg_channel, cfg_bw);
  }
  ROS_INFO("%s",configcmd);
  return sh_exec_block(configcmd);
}




/**
 * Ensure the remote device is up, optionally finding it with nmap.
 * This function sets g_this_device_user and g_this_device_IP.
 * If cfg_sta_ip was a wildcard (e.g. '192.168.43.*') it will
 * replace it with the resolved IP.
 */
void setup_asus_connection(){
  std::smatch ip_match;
  char subnet[128]; //should be max 16 bytes, large to avoid warning.
  bool scan=false;
  if(std::regex_search(cfg_sta_ip,ip_match,ip_ex)){
    sprintf(subnet, "%s.%s.%s.", ip_match[1].str().c_str(), ip_match[2].str().c_str(), ip_match[3].str().c_str());
    if(ip_match[4]=="*"){  
      scan=true;
    }
  }
  else{
    ROS_FATAL("Invalid target IP, needs to be xxx.xxx.xxx.xxx or xxx.xxx.xxx.*");
  }

  std::stringstream hostname_I_result(sh_exec_block("hostname -I"));
  std::string IP;
  bool iface_up = false;
  while(getline(hostname_I_result, IP, ' ')){
    if (IP.rfind(subnet, 0) == 0) {
      g_this_device_IP = IP;
      iface_up = true;
      if(scan){
	char cmd[256];
	ROS_INFO("Scanning for ASUS routers...");
	sprintf(cmd, "nmap -sP %s0/24", subnet);
	ROS_INFO("%s", cmd);
	std::stringstream nmap(sh_exec_block(cmd));
	std::string target;
	while(getline(nmap, target, ' ')){
	  if (target.rfind(subnet, 0) == 0){
	    std::string temp_ip = target.substr(0, target.find("\n"));
	    if(temp_ip != g_this_device_IP){
	      cfg_sta_ip = temp_ip;
	    }
	  }
	}
      }
    }
  }
  if(!iface_up){
    ROS_ERROR("Could not find an interface on which to reach %s",subnet);
    exit(1);
  }

  //Ping the AP to ensure everything is working good.
  char setupcmd[512];
  sprintf(setupcmd, "ping -c 3 -i 0.3 %s", cfg_sta_ip.c_str());
  std::string ping_result = sh_exec_block(setupcmd);
  ROS_INFO("%s", ping_result.c_str());
  if(ping_result.find("Destination Host Unreachable",0) != std::string::npos || ping_result.find("100% packet loss",0) != std::string::npos){
    ROS_ERROR("The host at %s did not respond to a ping.", cfg_sta_ip.c_str());
    ROS_ERROR("This is probably because the 'asus_ip' param is setup to the incorrect value.");
    ROS_ERROR("You can enable automatic ASUS detection by setting 'asus_ip' to \"\"");
    exit(1);
  }
  
  g_this_device_user = sh_exec_block("hostname");
}

void setup_beacon(){
  //mac address we will transmit on will be 11:11:11:first byte of name:second byte of name:last byte of ip4
  uint8_t mac4 = g_this_device_user[0];
  uint8_t mac5 = g_this_device_user[1];
  size_t pos = g_this_device_IP.rfind('.');
  uint8_t mac6 = (uint8_t)std::stoi(std::string(g_this_device_IP).erase(0,pos+1));
  ROS_INFO("Starting transmitter...");
  char setupcmd[512];
  sprintf(setupcmd, "sshpass -p %s ssh -o strictHostKeyChecking=no %s@%s /jffs/csi/send.sh %d %d %d %s 11 11 11 %x %x %x > /dev/null 2>&1",
          cfg_sta_pass.c_str(), cfg_sta_user.c_str(), cfg_sta_ip.c_str(), cfg_bw, cfg_sta_num_tx_cores,
          (int)cfg_beacon_period*1000, cfg_sta_bcm_iface.c_str(), mac4, mac5, mac6);
  ROS_INFO("%s", setupcmd);
  ROS_WARN("Beaconing on 11:11:11:%x:%x:%x",mac4,mac5,mac6);
  beacon_process_fp = popen(setupcmd, "r");

}


void setup_tcpdump(std::string hostIP){
  char setupcmd[512];
  sprintf(setupcmd, "sshpass -p %s ssh -o strictHostKeyChecking=no %s@%s /jffs/csi/tcpdump -i %s port 5500 -nn -s 0 -w - --immediate-mode | nc %s %d > /dev/null 2>&1", cfg_sta_pass.c_str(), cfg_sta_user.c_str(), cfg_sta_ip.c_str(), cfg_sta_bcm_iface.c_str(), hostIP.c_str(), PORT_TCP);
  ROS_INFO("%s",setupcmd);
  cli_fp = popen(setupcmd, "r");
}

void setup_params(ros::NodeHandle& nh){
  double tmp_ch, tmp_bw;
  std::string tmp_host;
  
  nh.param<double>("channel", tmp_ch, 157.0);
  nh.param<double>("bw", tmp_bw, 80.0);
  cfg_channel = (int)tmp_ch;
  cfg_bw = (int)tmp_bw;
  nh.param<double>("beacon_period", cfg_beacon_period, 200.0);
  nh.param<int>("beacon_tx_nss", cfg_sta_num_tx_cores, 4);
  nh.param<bool>("tcp_forward", cfg_use_tcp_transport, false);
  nh.param<std::string>("asus_ip", cfg_sta_ip, "");
  nh.param<std::string>("asus_pwd", cfg_sta_pass, "password");
  nh.param<std::string>("asus_host", cfg_sta_user, "HOST");
  nh.param<bool>("cfg_do_not_start_nexmon", cfg_do_not_start_nexmon, false);
  nh.param<std::string>("lock_topic", cfg_ap_scanner_topic, "");
  

  //MAC filter param
  std::string mac_filter_temp;
  nh.param<std::string>("mac_filter", mac_filter_temp, std::string(""));
  set_mac_filter(mac_filter_str(mac_filter_temp));
}


//handle change of channel, returns false on error.
bool config_csi_callback(wiros_csi_node::ConfigureCSI::Request &req, wiros_csi_node::ConfigureCSI::Response &resp){
  if(req.chan == cfg_channel && req.bw == cfg_bw){
    resp.result = "No Change Applied.";
    return true;
  }
  if(set_chanspec(req.chan, req.bw)){
    resp.result = "Error: Invalid Channel Or Bandwidth";
    return false;
  }
  if(req.mac_filter != "" && set_mac_filter(mac_filter_str(req.mac_filter))){
    resp.result = "Error: Invalid MAC Filter";
    return false;
  }
  resp.result = reset_nexmon();
  resp.result.erase(std::remove_if(resp.result.begin(),resp.result.end(), sanitize_string), resp.result.end());
  return true;
}

void ap_info_callback(const rf_msgs::AccessPoints::ConstPtr& msg){
  uint8_t a_mac[6];
  if(msg->aps.size() < 1) return;
  
  mac_filter filt;
  filt.len = 5;
  memcpy(filt.mac, msg->aps[0].mac.data(), 6);
  if(!set_mac_filter(filt) && !set_chanspec(msg->aps[0].channel, 20)){
    reset_nexmon();
  }

}


