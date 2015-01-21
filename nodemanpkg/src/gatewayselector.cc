#include <click/config.h>
#include <click/ipaddress.hh>
#include <click/args.hh>
#include <click/error.hh>
#include <click/glue.hh>
#include <click/straccum.hh>
#include <click/packet_anno.hh>
#include <clicknet/wifi.h>
#include <click/etheraddress.hh>
#include "gatewayselector.hh"

#include <string>

CLICK_DECLS

#define GATES_REFRESH_INTERVAL 60 // in seconds
#define STALE_ENTRY_THRESHOLD 60 //in seconds

std::string mac_to_string(uint8_t address[])
{
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
         address[0], address[1], address[2], address[3], address[4], address[5]);
  return std::string(macStr);
}

std::string ip_to_string(uint8_t ip[])
{
	char ipStr[16];
	sprintf(ipStr, "%d.%d.%d.%d",ip[0],ip[1],ip[2],ip[3]);
	return std::string(ipStr);
}

void string_to_mac(std::string mac_string, uint8_t address[])
{
  sscanf(mac_string.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
					(unsigned int *)&address[0], (unsigned int *)&address[1], (unsigned int *)&address[2], 
				  (unsigned int *)&address[3], (unsigned int *)&address[4], (unsigned int *)&address[5]);
}


GatewaySelector::GatewaySelector()
    : _print_anno(false),
      _print_checksum(false),
      _master_timer(this)
{
    _label = "";
}

GatewaySelector::~GatewaySelector()
{
}

int GatewaySelector::initialize(ErrorHandler *)
{
		_master_timer.initialize(this);
		_master_timer.schedule_now();
		return 0;
}

void GatewaySelector::run_timer(Timer *timer)
{
		assert(timer == &_master_timer);
		Timestamp now = Timestamp::now_steady();

		// clean_gates_table();
		// clean_cache_table();
		
		vector<GateInfo>::iterator it;
		for(it = gates.begin(); it != gates.end(); ++it) {
		  
		  if((*it.timestamp - time(NULL)) > STALE_ENTRY_THRESHOLD)
		    {
		      printf("Removing gate %s\n", *it->ip_address);
		      it = gates.erase(it);
		    }
		}

		vector<PortCache>::iterator iter;
		for(iter = port_cache_table.begin(); iter != port_cache_table.end(); ++iter) {
		  
		  if((*iter.timestamp - time(NULL)) > STALE_ENTRY_THRESHOLD)
		    {
		      printf("Removing entry for port no. %d\n", *iter->src_port);
		      iter = port_cache_table.erase(iter);
		    }
		}
		
		_master_timer.reschedule_after_sec(GATES_REFRESH_INTERVAL);		
}

int
GatewaySelector::configure(Vector<String> &conf, ErrorHandler* errh)
{
  int ret;
  _timestamp = false;
  ret = Args(conf, this, errh)
      .read_p("LABEL", _label)
      .read("TIMESTAMP", _timestamp)
      .complete();
  return ret;
}

void GatewaySelector::process_pong(Packet * p)
{
  // process pong here
  // 1. extract mac, ip and metric in pong
  // 2. look for mac as key in unresolved_gates map
  // 3. update the corresponding gate_info structure.
  // 4. Remove the gate_info struct from unresolved and put it in resolved.
	uint8_t src_mac[6], src_ip[4];	
	uint8_t *ptr = NULL;
	
	if(p->has_mac_header()) {
		ptr = (uint8_t *)p->mac_header();
		//Skip destination as it should be a broadcast address
		ptr+= 6;
		//Skip to source mac address
		for(int i=0; i<6; i++) {
			src_mac[i] = *ptr;
			ptr++;
		}

		//skip protocol code
		ptr+=2;
		//extract ipv4
		for(int i=0; i<4; i++) {
			src_ip[i] = *ptr;
			ptr++;
		}
		
		std::string src_mac_string = mac_to_string(dest_mac);
		std::string src_ip_string = ip_to_string(src_ip);
		
		printf("----Data from pong------\n");
		printf("src_mac: %s\nnsrc_ip: %s\n",					
					 src_mac_string.c_str(),
					 src_ip_string.c_str()
					);
		printf("------------------------\n");
		
		// Find this gate's entry using its mac address which is the source mac address
		
		std::vector<GateInfo>::iterator it;

		for(it = gates.begin(); it!=gates.end(); it++)
		  {
		    if(*it.mac_address == src_mac_string)
		      {
			if(*it.ip_address != src_ip_string)
			  {			
			    printf("Warning: IP address changed from %s to %s for host MAC %s\n",
				   *it.ip_address.c_str(), src_ip_string.c_str(),
				   src_mac_string.c_str());
			    
			    *it,ip_address = src_ip_string; 
			  }
			
			*it.timestamp = time(NULL);
			break;
		      }
		  }

		//New gate discovered
		if(it == gates.end()) {
		  GateInfo *new_gate = new GateInfo;
		  new_gate.ip_address = src_ip_string;
		  new_gate.mac_address = src_mac_address;
		  new_gate.timestamp = time(NULL);
		  
		  // put metrics when extending this function here

		  gates.push_back(new_gate);		  
		}

		//Printing the list of gates. Drop this later.
		printf("gates(%d):\n",gates.size());
		for(it = gates.begin(); it!=gates.end(); ++it) {
		  printf("%s -> %s\n",(*it -> mac_address).c_str(), (*it -> ip_address).c_str());
		}				
	}
	else
	  printf("Malformed packet received without header!\n");		
}


void GatewaySelector::push(int port, Packet *p)
{
  switch(port)
    {
    case 0: /* Normal packet for setting the gateway */
      p = select_gate(p);
      output(0).push(p);
      break;
      
    case 1:
      process_pong(p);    
      break;
    }
}

Packet *select_gate(Packet *p)
{
  IPAddress ip;
  uint16_t *ptr = p->data();
  ptr += 2;
  uint16_t src_port = *ptr;
  ip = cache_lookup(src_port);
  if(ip == 0.0.0.0)
  {
    ip = find_gate(src_port);
    cache_update(src_port,ip);
  }
  p = set_ip_address(p,ip);
  return p;
}

IPAddress cache_lookup(uint16_t src_port)
{
  std::vector<port_cache_table>::iterator it = cache_table.begin();
  while(it ! = cache_table.end())
  {
    if(it.src_port == src_port)
      return it.gate_ip;
  }
  
  return 0.0.0.0;    
}

Packet *set_ip_address(Packet *p, IPAddress ip)
{
  p->set_anno_u32(_anno,ip);
  return p;
}

IPAddress find_gate(uint16_t src_port)
{
  int index = src_port % gates.size();
  return gates[index].ip_address;
}

void cache_update(uint16_t src_port, IPAddress ip)
{
  PortCache entry;
  entry.src_port = src_port;
  entry.gate_ip = ip;
  entry.timestamp = 0;
  cache_table.push_back(entry);
}

CLICK_ENDDECLS
EXPORT_ELEMENT(GatewaySelector)
