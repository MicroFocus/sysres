/*****************************************************************************
* sysres "System Restore" Partition backup and restore utility.
* Copyright © 2019-2020 Micro Focus or one of its affiliates.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along
* with this program; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifdef NETWORK_ENABLED
#include <fcntl.h>
#include <ifaddrs.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/if_ether.h> 	// used for dhcp request
#include <netinet/in.h>
#include <net/if.h>
#include <net/route.h>
#include <netpacket/packet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "mount.h"				// currentLine
#include "partition.h"
#include "window.h"    		// options, currentFocus

#define IPLEN 15
#define STATLEN 4 // UP / DOWN (no link == dim)

#define NETWIDTH 40
#define BARWIDTH 37

#define NETTITLE "NETWORK SETTINGS"

#define INTF_UP 1
#define HAS_LINK 2

int top = 0;

typedef struct __interface interface;

struct __interface { // IPv4 only for now
	int state;	// LINK, UP	'up *', 'down *'
	unsigned char name[IFNAMSIZ];
	struct in_addr ip;
	struct in_addr mask;
	struct in_addr gw;
	struct in_addr dhcpserv;
	unsigned int leaseTime;
	unsigned char mac[6];
        };

#define MAXINTF 8
interface interfaces[MAXINTF];
int interfaceCount = 0;
int currentSelection = 0;

void showNetwork(bool edit) {
        int i;
        mvaddch(LINES-1,0,' '); clrtoeol(); // for (i=0;i<COLS;i++) addch(' '); // clear the menu line
//	showFunction(0,"F1","UP",0);
	if (!edit) showFunction(0,"F1","DONE",0);
	if (interfaceCount) showFunction(COLS-(54+9),"F6",(edit)?"CANCEL":"EDIT",0);
        // showFunction(COLS-(54+9),"F6","CANCEL",0);
        // if (mkdir) showFunction(0,"/","MKDIR",0);
        }

// dhcp variables
int send_sock;
int recv_sock;
struct sockaddr_in dhcps;
struct sockaddr_ll saddr;
const char magic[] = { 0x63, 0x82, 0x53, 0x63 };
unsigned int xid = 0;
#define OFF_ADDR 16
#define OFF_MAC 28
#define OFF_MAGIC 236
#define OFF_OPTIONS 240 // OFF_MAGIC + 4

#define SIOCGMIIPHY 0x8947
#define SIOCGMIIREG 0x8948
#define MII_BMSR 0x01
#define MII_BMSR_LINK_VALID 0x0004

#include <linux/types.h>
struct mii_data {
    __u16       phy_id;
    __u16       reg_num;
    __u16       val_in;
    __u16       val_out;
};

void getGateway(char *intf) {
        // SIOCGRTCONF
        // read /proc/net/route, match intf and dest of 0000000
        char *tok;
        int i;
        struct in_addr z;
	interface *intfs = &interfaces[interfaceCount];
	memset(&intfs->gw,0,sizeof(struct in_addr)); // no GW unless found
	char byte[3];
	byte[2] = 0;

        if (openFile("/proc/net/route")) return;
        while(readFileLine()) {
                if ((tok = strtok(currentLine,"\t")) == NULL) continue;
                if (strcmp(tok,intf)) continue;
                if ((tok = strtok(NULL,"\t")) == NULL) continue;
                if (strcmp(tok,"00000000")) continue;
                if ((tok = strtok(NULL,"\t")) == NULL) continue;
                if (strlen(tok) != 8) continue;
                z.s_addr = 0;
/*		// reverse
                for (i=4;i>0;i--) {
                        tok[6] = tok[i*2-2];
                        tok[7] = tok[i*2-1];
                        z.s_addr <<= 8; // 1 byte
                        z.s_addr += strtol(&tok[6],NULL,16);
                        }
*/
		for(i=0;i<4;i++) {
			memcpy(byte,&tok[i*2],2);
			z.s_addr += strtol(byte,NULL,16) << (3-i)*8;
			}
		memcpy(&intfs->gw,&z,sizeof(struct in_addr));
                }
        }

bool hasLink(char *intf) {
	int sock;
	struct ifreq ifr;
	if ((sock = socket(AF_INET,SOCK_DGRAM,0)) < 0) return false;
        strncpy(ifr.ifr_name,intf,IFNAMSIZ);
	struct mii_data *mii = (struct mii_data *)&ifr.ifr_data;
        if (ioctl(sock,SIOCGMIIPHY,&ifr) >= 0) { // can only check this on something that is IF_UP...?
                mii->reg_num = MII_BMSR;
                if ((ioctl(sock,SIOCGMIIREG,&ifr) >= 0) && (mii->val_out & MII_BMSR_LINK_VALID)) { close(sock); return true; }
                }
	close(sock);
	return false;
	}


void checkInterface(char *intf) {
	int sock;
	struct ifreq ifr;
	interface *intfs = &interfaces[(intf != NULL)?interfaceCount:currentSelection];
	if (intf == NULL) intf = intfs->name;

	if (!strcmp(intf,"sit0") || !strcmp(intf,"lo")) return;

	if ((sock = socket(AF_INET,SOCK_DGRAM,0)) < 0) return;
	strncpy(ifr.ifr_name,intf,IFNAMSIZ);

	// check link (must be root; otherwise it'll report as 0
	intfs->state = 0;
	if (hasLink(intf)) intfs->state = HAS_LINK;
/*
	struct mii_data *mii = (struct mii_data *)&ifr.ifr_data;
        if (ioctl(sock,SIOCGMIIPHY,&ifr) >= 0) { // can only check this on something that is IF_UP...?
        	mii->reg_num = MII_BMSR;
        	if ((ioctl(sock,SIOCGMIIREG,&ifr) >= 0) && (mii->val_out & MII_BMSR_LINK_VALID)) intfs->state = HAS_LINK;
		}
*/
	if (ioctl(sock,SIOCGIFADDR,&ifr) < 0) { // no address
		if (ioctl(sock,SIOCGIFHWADDR,&ifr) < 0) { close(sock); return; } // no MAC address
		memcpy(intfs->mac,&ifr.ifr_hwaddr.sa_data,6);
		memset(&intfs->ip,0,sizeof(struct in_addr));
		memset(&intfs->mask,0,sizeof(struct in_addr));
		memset(&intfs->gw,0,sizeof(struct in_addr));
		}
        else {
		memcpy(&intfs->ip,&(*(struct sockaddr_in *)&ifr.ifr_addr).sin_addr,sizeof(struct in_addr));
		if (ioctl(sock,SIOCGIFNETMASK,&ifr) < 0) { close(sock); return; }
		memcpy(&intfs->mask,&(*(struct sockaddr_in *)&ifr.ifr_netmask).sin_addr,sizeof(struct in_addr));
		getGateway(intf);
                if (ioctl(sock,SIOCGIFHWADDR,&ifr) < 0) { close(sock); return; } // MAC address
		memcpy(intfs->mac,&ifr.ifr_hwaddr.sa_data,6);
//		if (ioctl(sock,SIOCGIFMTU,&ifr) < 0) { close(sock); return; }
//		printf(" [%i]",ifr.ifr_mtu);
                }
	if (ioctl(sock,SIOCGIFFLAGS,&ifr) < 0) return;
        if (ifr.ifr_flags & IFF_UP) intfs->state |= INTF_UP;
	close(sock);
	if ((char *)intfs->name != (char *)intf) {
		intfs->leaseTime = 0;
		bzero(&intfs->dhcpserv,sizeof(struct in_addr));
		strncpy(intfs->name,intf,IFNAMSIZ);
		interfaceCount++;
		}
	}

void populateInterfaces(void) {
        struct ifaddrs *ifaddr, *ifa;
        int family;
	int i;
	interface *intf;
	if (!interfaceCount) {
        	if (getifaddrs(&ifaddr) == -1) {  debug(INFO,5,"getifaddr()\n"); return; }
        	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
                	if (ifa->ifa_addr == NULL) continue;
               		 family = ifa->ifa_addr->sa_family;
                	if (family != AF_PACKET) continue;
			checkInterface(ifa->ifa_name);
                	}
        	freeifaddrs(ifaddr);
		}
	else {
		currentSelection = interfaceCount-1;
		do { checkInterface(NULL); } while (currentSelection--);
		currentSelection = 0;
		}
        }

void tempPop(void) {
	int i;
	interface *intf;
	interfaceCount = MAXINTF;
	for (i=0;i<interfaceCount;i++) {
		intf = &interfaces[i];
		intf->state = INTF_UP;
		sprintf(intf->name,"eth%i",i);
		inet_pton(AF_INET,"192.168.101.100",&intf->ip);
		inet_pton(AF_INET,"255.255.255.252",&intf->mask);
		inet_pton(AF_INET,"192.168.101.101",&intf->gw);
		}
	}

void showNets(WINDOW *netwin, int y, int x,bool currentOnly) {
	int i;
	char lineBuf[LINELIMIT];
	char *ip;
	interface *a;
	char ipBuf[16];
	if (top) mvwaddch(netwin,4,x-3,ACS_UARROW);
	else mvwaddch(netwin,4,x-3,' ');
	for (i=top;i<interfaceCount;i++) {
		if (currentOnly && (i != currentSelection)) continue;
		if ((i-top)+7 > y) {
			mvwaddch(netwin,y-3,x-3,ACS_DARROW);
			break;
			}
		a = &interfaces[i];
		memset(lineBuf,' ',COLS-8);
		// lineBuf[BARWIDTH-1] = 0;
if (!(a->state & INTF_UP)) wattron(netwin,COLOR_PAIR(CLR_WDIM));
                if (i == currentSelection) wattron(netwin, A_REVERSE);
		strncpy(&lineBuf[1],a->name,strlen(a->name) > (IFNAMSIZ-4)?(IFNAMSIZ-4):strlen(a->name));
		ip = &lineBuf[IFNAMSIZ-3];
		inet_ntop(AF_INET,&a->ip,ipBuf,16);
		sprintf(ip,"%15s",strcmp(ipBuf,"0.0.0.0")?ipBuf:" ");
		ip[strlen(ip)] = ' ';
		ip = &lineBuf[IFNAMSIZ+IPLEN+1];
		inet_ntop(AF_INET,&a->mask,ipBuf,16);
		sprintf(ip,"%15s",strcmp(ipBuf,"0.0.0.0")?ipBuf:" ");
		ip[strlen(ip)] = ' ';
		ip = &lineBuf[IFNAMSIZ+IPLEN+IPLEN+4];
		inet_ntop(AF_INET,&a->gw,ipBuf,16);
		sprintf(ip,"%15s",strcmp(ipBuf,"0.0.0.0")?ipBuf:" ");
                ip[strlen(ip)] = ' ';
		ip = &lineBuf[IFNAMSIZ+IPLEN*3+7];
		sprintf(ip,"%s",(a->state & INTF_UP)?(a->state & HAS_LINK)?"YES":" NO":"---");
		ip[3] = ' ';
		lineBuf[COLS-8] = 0;
                mvwprintw(netwin,4+(i-top),4,lineBuf);
                if (i == currentSelection) {
			wattroff(netwin, A_REVERSE);
			mvwprintw(netwin,1,1,"%02X:%02X:%02X:%02X:%02X:%02X",a->mac[0],a->mac[1],a->mac[2],a->mac[3],a->mac[4],a->mac[5]);
showFunction(COLS-(45+9),"F7",(a->state & INTF_UP)?"DOWN":"UP  ",0);
if (!(a->state & INTF_UP)) showFunction(COLS-(36+9),"F8","DHCP",0) ;
else mvprintw(LINES-1,COLS-(36+9),"      ");
			}
if (!(a->state & INTF_UP)) wattroff(netwin,COLOR_PAIR(CLR_WDIM));
                }
	if (i == interfaceCount) mvwaddch(netwin,y-3,x-3,' ');
	wrefresh(netwin);
	}

int fieldStart(char *field) {
        int n = 0;
        while(*field == ' ') { field++; n++; }
        return n;
        }

bool addGateway(int sock, struct ifreq *ifr, char *gateway) {
        struct rtentry rm;
        struct sockaddr_in gw, *dst, *mask;
        memset(&rm,0,sizeof(rm));
	rm.rt_dev = ifr->ifr_name; // identifies interface
        gw.sin_family = AF_INET;
        gw.sin_addr.s_addr = inet_addr(gateway);
        gw.sin_port = 0;
        dst = (struct sockaddr_in *)&rm.rt_dst;
        mask = (struct sockaddr_in *)&rm.rt_genmask;
        dst->sin_family = mask->sin_family = AF_INET;
        dst->sin_addr.s_addr = mask->sin_addr.s_addr = 0;
        dst->sin_port = mask->sin_port = 0;
        memcpy(&rm.rt_gateway,&gw,sizeof(gw));
        rm.rt_flags = RTF_UP | RTF_GATEWAY;
        if (ioctl(sock,SIOCADDRT,&rm) < 0) return false; // check for success here...
	return true;
        }

bool toggleIntf(int sock, struct ifreq *ifr, bool activate) {
        int i, j = 0;
	if (activate) ifr->ifr_flags |= IFF_UP | IFF_RUNNING; // set flags
	else ifr->ifr_flags &= ~(IFF_UP | IFF_RUNNING);  // clear flags
	if (ioctl(sock,SIOCSIFFLAGS,ifr) < 0) return false; // set
        for (i=0;i<100;i++) {
                if (ioctl(sock,SIOCGIFFLAGS,ifr) < 0) return false;
		if (ifr->ifr_flags & IFF_UP) {
			if (activate) {
				// sleep(1); // give it a second
				return true;
				}
			}
		else {
			if (!activate) return true;
			}
		usleep(10000);
		}
	if (!activate) return false; // didn't go down
        ifr->ifr_flags &= ~(IFF_UP | IFF_RUNNING);
        ioctl(sock,SIOCSIFFLAGS,ifr); // bring back down
	return false;
	}

bool quickToggle(interface *intfs, bool bringUp) {
	int sock;
	struct ifreq ifr;
	bzero(&ifr,sizeof(ifr));
	if ((sock = socket(AF_INET,SOCK_DGRAM,0)) < 0) return false;
	strncpy(ifr.ifr_name,intfs->name,IFNAMSIZ);
        ifr.ifr_addr.sa_family = AF_INET;
	return toggleIntf(sock,&ifr,bringUp);
	}

bool configureInterface(interface *intfs, char *ip, char *mask, char *gw, WINDOW *win) {
        int sock;
	int t;
        struct ifreq ifr;
        struct sockaddr_in *addr;
        if ((sock = socket(AF_INET,SOCK_DGRAM,0)) < 0) return false;
        strncpy(ifr.ifr_name,intfs->name,IFNAMSIZ);
        ifr.ifr_addr.sa_family = AF_INET;
        addr = (struct sockaddr_in *)&ifr.ifr_addr;
	if (win != NULL) {
		if (!toggleIntf(sock,&ifr,false)) { debug(INFO,5,"DOWN\n"); close(sock); return false; } // bring down (automatically removes gateway)
		intfs->state &= ~INTF_UP;
		}
	if (ip[14] == ' ') { memset(ip,' ',15); strcpy(&ip[8],"0.0.0.0"); } // inet_pton(AF_INET,"0.0.0.0",&addr->sin_addr); } // disable ip/mask, remove GW
	t = fieldStart(ip);
	inet_pton(AF_INET,&ip[t],&addr->sin_addr);
	debug(INFO,5,"Setting address %s\n",&ip[t]);
        if (ioctl(sock,SIOCSIFADDR,&ifr) < 0) {
		debug(INFO,5,"SIOCSIFADDR\n");
		close(sock); return false;
		}
	if (!memcmp(&addr->sin_addr,"\00\00\00\00",4)) { // reset all; bring intf up regardless
		memset(mask,' ',15);
		strcpy(&mask[8],"0.0.0.0");
		memset(gw,' ',15);
		strcpy(&gw[8],"0.0.0.0");
		// return true;
		} // reset
	else {
		t = fieldStart(mask);
        	inet_pton(AF_INET,&mask[t],&addr->sin_addr);
        	if (ioctl(sock,SIOCSIFNETMASK,&ifr) < 0) { debug(INFO,5,"SIOCIFNETMASK\n"); close(sock); return false; } // check for success here...
		debug(INFO,5,"Setting mask %s\n",&mask[t]);
		}
        if (!toggleIntf(sock,&ifr,true)) { debug(INFO,5,"UP\n"); close(sock); return false; } // bring up interface to set gateway
	if (win != NULL) {
		mvwprintw(win,1,COLS-14,"WAITLINK    "); wrefresh(win);
		t = 0;
		while((t < 1000) && !hasLink(intfs->name)) { usleep(10000); t++; }  // wait for it to come up
        	mvwprintw(win,1,COLS-14,"            "); wrefresh(win);
		if (t == 1000) { intfs->state &= ~HAS_LINK; return false; }
		else intfs->state |= HAS_LINK;
		intfs->state |= INTF_UP;
		}
	if (((t = fieldStart(gw)) != 15) && (strcmp(&gw[t],"0.0.0.0"))) { // not blank
        	if (!addGateway(sock,&ifr,&gw[t])) { debug(INFO,5,"GATEWAY [%s]\n",&gw[t]); close(sock); return false; } ; // check for sucess here...
		debug(INFO,5,"Setting gateway %s\n",&gw[t]);
		}
        close(sock);
	return true;
        }

void populateLocal(interface *intfs, char *ip, char *mask, char *gw) {
	char ipBuf[16];
        inet_ntop(AF_INET,&intfs->ip,ipBuf,16);
        sprintf(ip,"%15s",(strcmp(ipBuf,"0.0.0.0"))?ipBuf:" ");
        inet_ntop(AF_INET,&intfs->mask,ipBuf,16);
        sprintf(mask,"%15s",(strcmp(ipBuf,"0.0.0.0"))?ipBuf:" ");
        inet_ntop(AF_INET,&intfs->gw,ipBuf,16);
        sprintf(gw,"%15s",(strcmp(ipBuf,"0.0.0.0"))?ipBuf:" ");
	}

bool startSession(char *intf) {
        int flag = 1;
        struct ifreq ifr;
        memset(&ifr,0,sizeof(ifr));
        strncpy(ifr.ifr_name,intf,IFNAMSIZ);
        dhcps.sin_family = AF_INET;
        dhcps.sin_port=htons(67); // bootps port 67
        dhcps.sin_addr.s_addr=INADDR_BROADCAST; // INADDR_NONE;
        if ((send_sock = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP)) < 0) {  debug(INFO,5,"DHCP socket error.\n"); return false; }
        ioctl(send_sock,SIOCGIFINDEX,&ifr);
        if (setsockopt(send_sock,SOL_SOCKET,SO_BINDTODEVICE,(void *)&ifr,sizeof(ifr)) < 0) { debug(INFO,5,"DHCP Bind device error.\n"); close(send_sock); return false; }
        else debug(INFO,5,"DHCP binding to %s.\n",ifr.ifr_name);
        if (setsockopt(send_sock,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag)) < 0) { debug(INFO,5,"DHCP REUSEADDR error.\n"); close(send_sock); return false; }
        if (setsockopt(send_sock,SOL_SOCKET,SO_BROADCAST,&flag,sizeof(flag)) < 0) { debug(INFO,5,"DHCP BROADCAST error.\n"); close(send_sock); return false; }
        /* receiver */
        if ((recv_sock = socket(PF_PACKET,SOCK_DGRAM,htons(ETH_P_ALL))) < 0) { debug(INFO,5,"DHCP listen error.\n"); close(send_sock); return false; }
        if (ioctl(recv_sock,SIOCGIFINDEX,&ifr) < 0) { debug(INFO,5,"DHCP IFINDEX error.\n"); close(send_sock); close(recv_sock); return false; }
        // toggleIntf(recv_sock,&ifr,true); // make sure it's up (only bring it back down if no address was delivered)
        if (setsockopt(recv_sock,SOL_SOCKET,SO_BINDTODEVICE,(void *)&ifr,sizeof(ifr)) < 0) { debug(INFO,5,"DHCP BINDTODEVICE error.\n"); close(send_sock); close(recv_sock); return false; }
        saddr.sll_family = AF_PACKET;
        saddr.sll_ifindex = ifr.ifr_ifindex;
        saddr.sll_protocol = ETH_P_ALL;
	return true;
        }

void sendRequest(interface *intf, bool release) { // dhcp
        unsigned char buf[512];
        int optSize;
        memset(buf,0,sizeof(buf));              // offset from packet capture: [28]
        sprintf(buf,"%c%c%c%c",1,1,6,0);        // dhcp header
        if (!intf->leaseTime) {
                if (xid > time(NULL)) xid++;
                else xid = time(NULL);
                }
        memcpy(&buf[4],&xid,4); // xid          // 48 5b 7b 49
        memcpy(&buf[OFF_MAC],intf->mac,6);
        memcpy(&buf[OFF_MAGIC],magic,4);
        if (intf->leaseTime) { // DHCPREQUEST
                sprintf(&buf[OFF_OPTIONS],"%c%c%c",0x35,1,(release)?7:3); // type 3 == request, 7 == release
                sprintf(&buf[OFF_OPTIONS+3],"%c%c0000%c%c",0x36,4,0x32,4); // server identifier, client identifier
                memcpy(&buf[OFF_OPTIONS+5],&intf->dhcpserv,4);
                memcpy(&buf[OFF_OPTIONS+11],&intf->ip,4);
                buf[OFF_OPTIONS+15] = 0xFF;
                optSize = OFF_OPTIONS+16;
                }
        else { // DHCPDISCOVER
                sprintf(&buf[OFF_OPTIONS],"%c%c%c",0x35,1,1); // type 1 == discover
                sprintf(&buf[OFF_OPTIONS+3],"%c%c%c%c",0x37,2,1,3); // param. req. list, 2 params, subnet, router list
                buf[OFF_OPTIONS+7] = 0xFF;
                optSize = OFF_OPTIONS+8;
                }
        sendto(send_sock,buf,optSize,0,(struct sockaddr *)&dhcps,sizeof(dhcps));
	debug(INFO,5,"Sending DHCP%s\n",(intf->leaseTime)?(release)?"RELEASE":"REQUEST":"DISCOVER");
        }

void displayFour(char *msg, unsigned char *buf) {
        char ipBuf[17];
        inet_ntop(AF_INET,buf,ipBuf,16);
	debug(INFO,5,"%s%s\n",msg,ipBuf);
        // printf("%s%i.%i.%i.%i\n",msg,buf[0],buf[1],buf[2],buf[3]);
        }

bool readRaw(interface *intf) {
        int i;
        unsigned char ip[4], gw[4], mask[4], dhcpserv[4];
        unsigned int lease;
        int flag;
        unsigned int offset, port, length;
        unsigned char *option;
        unsigned char buf[1024];
        unsigned char *dhcpmsg;
        int z = sizeof(saddr);
        int optlen;
        fd_set rd;
        struct timeval timeout;
        timeout.tv_usec = 0;
        timeout.tv_sec = 10;
        int startTime = time(NULL);
        int elapsed;
        unsigned int rxid;
        int origLen;
        bool valid = false;
        int msgType = 0;
	bzero(ip,4); bzero(gw,4); bzero(mask,4); bzero(dhcpserv,4);
        while (true) {
                if ((elapsed = time(NULL) - startTime) >= 10) { debug(INFO,5,"DHCP general timeout %i\n",elapsed); return false; }
                timeout.tv_sec = 10 - elapsed;
                FD_ZERO(&rd);
                FD_SET(recv_sock,&rd);
                if (select(recv_sock+1,&rd,NULL,NULL,&timeout) < 0) { debug(INFO,5,"DHCP select error\n"); return false; }
                if (!FD_ISSET(recv_sock,&rd)) { debug(INFO,5,"DHCP select timeout\n"); return false; }
                if ((i = recvfrom(recv_sock,buf,1024,0,(struct sockaddr *)&saddr,&z)) < 0) continue;
                if (i < 10) continue;
                if (buf[9] != IPPROTO_UDP) continue; // 17
                offset = (*buf & 0xF)*4;
                if (i < offset + 8) continue;
                port = (buf[offset+2] << 8) + buf[offset+3];
                if (port != 68) continue;
                length = (buf[offset+4] << 8) + buf[offset+5];
                // if (buf[offset+6] || buf[offset+7]) { offset += 12; length -= 12; }  // extended checksum data does not appear to be supported
                offset += 8; length -= 8;
                dhcpmsg = &buf[offset];
                origLen = length;
                if (length + offset != i) { debug(INFO,5,"UDP length mismatch\n"); continue; }
                if (length < OFF_OPTIONS) { debug(INFO,5,"UDP too short\n"); continue; }
                memcpy(&rxid,&dhcpmsg[4],4);
                if (xid != rxid) { debug(INFO,5,"UDP XID mismatch\n"); continue; }
                if (memcmp(&dhcpmsg[OFF_MAC],intf->mac,6)) { debug(INFO,5,"MAC mismatch %02X:%02X:%02X:%02X:%02X:%02X\n",dhcpmsg[OFF_MAC],dhcpmsg[OFF_MAC+1],dhcpmsg[OFF_MAC+2],dhcpmsg[OFF_MAC+3],dhcpmsg[OFF_MAC+4],dhcpmsg[OFF_MAC+5]);
                        continue; } // mac mismatch
                if (memcmp(&dhcpmsg[OFF_MAGIC],magic,4)) { debug(INFO,5,"DHCP reply no magic\n"); continue; }
                memcpy(ip,&dhcpmsg[OFF_ADDR],4);
                // displayFour("Client address: ",&dhcpmsg[OFF_ADDR]);
                // if (buf[offset+28+16+64]) printf("\033[31mFile: %s\033[0m\n",&buf[offset+28+16+64]);
                option = &dhcpmsg[OFF_OPTIONS];
                length -= OFF_OPTIONS;
                valid = true;
                while (true) {
                        if (!length) { debug(INFO,5,"UDP out of space\n"); break; }
                        if (*option == 255) { /* printf("Done.\n"); */ break; }  // done
                        else if (!*option) { option++; length--; continue; }  // pad option
                        if ((length < 2) || (length < 2+(optlen = option[1]))) { debug(INFO,5,"UDP out of space\n"); break; }
                        length -= 2+optlen;
                        switch(*option) {
                                case 0x01:
                                        if (optlen != 4) return false;
                                        memcpy(mask,&option[2],4);
                                        break;
                                case 0x03: // router option
                                        if (!optlen) break; // no routers presented
                                        if (optlen % 4) return false;
                                        memcpy(gw,&option[2],4);
                                        break;
                                case 0x33: // 51 ip address lease time
                                        if (optlen != 4) return false;
                                        lease = (option[2] << 24) + (option[3] << 16) + (option[4] << 8) + option[5];
                                        break;
                                case 0x35: // 53 dhcp message type
                                        if (optlen != 1) return false;
                                        msgType = option[2];
                                        break;
                                case 0x36: // 54 server identifier
                                        if (optlen != 4) return false;
                                        memcpy(dhcpserv,&option[2],4);
                                        if (intf->leaseTime && (memcmp(dhcpserv,&intf->dhcpserv,4))) valid = false; // wrong one
                                        break;
                                default: break; // unknown option
                                }
                        option += 2+optlen;
                        }
/*
for (i=0;i<origLen;i++) {
        printf("%02X ",dhcpmsg[i]);
        if (i % 16 == 15) printf("\n");
        }
*/
                if (valid) {
                        if (msgType == 2) displayFour("Received DHCPOFFER from ",dhcpserv);
                        else if (msgType == 5) displayFour("Received DHCPACK from ",dhcpserv);
                        else displayFour("Received unknown response from ",dhcpserv);
                        memcpy(&intf->ip,ip,4);
                        memcpy(&intf->gw,gw,4);
                        memcpy(&intf->mask,mask,4);
                        memcpy(&intf->dhcpserv,dhcpserv,4);
                        intf->leaseTime = lease;
			return true; // should be done now (could also process additional reponses on the future)
			break;
                        }
                else { displayFour("Received extra DHCP response from ",dhcpserv); }
                }
	return false;
        }

void getLease(WINDOW *netwin) {
	interface *intf = &interfaces[currentSelection];
	char ip[4];
	char mask[4];
	char gw[4];
	char cip[16];
	char cmask[16];
	char cgw[16];
	bool good = false;
        struct ifreq ifr; // in case we have to bring it back down
	memcpy(ip,&intf->ip,4);
	memcpy(mask,&intf->mask,4);
	memcpy(gw,&intf->gw,4);
	bzero(&intf->dhcpserv,4);
	intf->leaseTime = 0;
	// mvwprintw(netwin,1,COLS-14,"DHCPDELAY   "); wrefresh(netwin); sleep(3);
	if (!startSession(intf->name)) return; // error
	configureInterface(intf,"0.0.0.0",cmask,cgw,netwin); // must have IP configured
	mvwprintw(netwin,1,COLS-14,"DHCPDISCOVER"); wrefresh(netwin);
	sendRequest(intf,false); // DHCPDISCOVER
	if (readRaw(intf)) { // got a response
		memcpy(&dhcps.sin_addr.s_addr,&intf->dhcpserv,4);
		mvwprintw(netwin,1,COLS-14,"DHCPREQUEST "); wrefresh(netwin);
		sendRequest(intf,false); // DHCPREQUEST
		if (readRaw(intf)) {
			populateLocal(intf,cip,cmask,cgw);
			if (configureInterface(intf,cip,cmask,cgw,NULL)) good=true;
			}
		}
	mvwprintw(netwin,1,COLS-14,"            "); wrefresh(netwin);
	if (!good) { // it failed; bring it down
		quickToggle(intf,false);
		intf->leaseTime = 0;
		memcpy(&intf->ip,ip,4);
		memcpy(&intf->gw,gw,4);
		memcpy(&intf->mask,mask,4);
		intf->state = 0;
		}
        close(send_sock);
        close(recv_sock);
	}

void releaseLease(void) {
	interface *intf = &interfaces[currentSelection];
	if (!intf->leaseTime) return; // not a lease
	if (!startSession(intf->name)) return; // error
	sendRequest(intf,true);
	close(send_sock);
	close(recv_sock);
	}

void editField(WINDOW *win, int vpos, interface *intfs) {
	int ch = 0;
	char fpos = 0; // 1 = mask, 2 = gw

	char *field;
	char ip[16];
	char mask[16];
	char gw[16];
	char ipBuf[16];
	field = ip;

	struct in_addr tmpAddr;
	int t;
	char *tmp;

	populateLocal(intfs,ip,mask,gw);

	int hpos = IFNAMSIZ+1;
	int cpos = 15;
	int cbuf = 0;

	char lastPos = cpos+hpos;
	char lastChar = ' ';

	bool moveField = false;

	int dimColor = (intfs->state & INTF_UP)?0:COLOR_PAIR(CLR_WDIM);

	do {
		switch(ch) {
			case KEY_F(6): goto DONE; break;
			case KEY_BTAB: case '\t': case KEY_ENTER: case 10:
				t = fieldStart(field);
				if ((t == 15) || (inet_pton(AF_INET, &field[t], &tmpAddr) > 0)) { // blank or valid
					switch(fpos) {
						case 0: strcpy(ip,field); break;
						case 1: strcpy(mask,(t == 15)?"  255.255.255.0":field);
							if (t == 15) { // auto-populate the blank field to class-C default
                        					wattron(win,dimColor | A_REVERSE);
                        					mvwprintw(win,vpos,IFNAMSIZ+IPLEN+5,"%s",mask);
                        					wattroff(win,dimColor | A_REVERSE);
								}
							break;
						default: strcpy(gw,field); break;
						}
					if ((fpos == 2) && (ch == KEY_ENTER || ch == 10)) {
						if (configureInterface(intfs,ip,mask,gw,win)) {
							t = fieldStart(ip);
					                inet_pton(AF_INET,&ip[t],&intfs->ip);
							t = fieldStart(mask);
                					inet_pton(AF_INET,&mask[t],&intfs->mask);
							t = fieldStart(gw);
                					inet_pton(AF_INET,&gw[t],&intfs->gw);
							}
						else {
							populateLocal(intfs,ip,mask,gw);
							configureInterface(intfs,ip,mask,gw,win); // revert to original settings
							}
						if (interfaces[currentSelection].state & INTF_UP) {
                                                	mvwprintw(win,1,COLS-14,"WAITLINK    "); wrefresh(win);
                                                	t = 0;
                                                	while((t < 1000) && !hasLink(interfaces[currentSelection].name)) { usleep(10000); t++; }  // wait for it to come up (spanning tree, switch, etc.)
                                                	mvwprintw(win,1,COLS-14,"            "); wrefresh(win);
                                                	if (t == 1000) interfaces[currentSelection].state &= ~HAS_LINK;
                                                	else interfaces[currentSelection].state |= HAS_LINK;
							}
						goto DONE;
						}
					else if (ch == KEY_BTAB) fpos = (fpos)?fpos-1:2;
					else fpos = (fpos == 2)?0:fpos+1;
					moveField = true;
					}
				break;
			case KEY_LEFT: if ((cpos) && field[cpos-1] != ' ') cpos--; break;
			case KEY_RIGHT: if (cpos < 15) cpos++; break;
			case KEY_BACKSPACE:
				if ((cpos) && field[cpos-1] != ' ') {
					memmove(&field[1],field,cpos-1);
					*field = ' ';
					wattron(win,dimColor | A_REVERSE);
					mvwprintw(win,vpos,hpos,"%s",field);
					wattroff(win,dimColor | A_REVERSE);
					}
				break;
			case 8: // shift backspace; clear everything to the left
				if ((cpos) && field[cpos-1] != ' ') {
					memset(field,' ',cpos);
                                        wattron(win,dimColor | A_REVERSE);
                                        mvwprintw(win,vpos,hpos,"%s",field);
                                        wattroff(win,dimColor | A_REVERSE);
					}
				break;
			case 330: // del
				if (cpos < 15) {
					memmove(&field[1],field,cpos);
					*field = ' ';
                                        wattron(win,dimColor | A_REVERSE);
                                        mvwprintw(win,vpos,hpos,"%s",field);
                                        wattroff(win,dimColor | A_REVERSE);
					cpos++;
					lastPos = cpos+hpos;
					lastChar = (cpos < 15)?field[cpos]:0;
					}
				break;
			default:
				if (*field == ' ' && ((ch >= '0' && ch <= '9') || ch == '.')) { // insert character
					if (cpos > 1) {
						memmove(field,&field[1],cpos-1);
						}
					field[cpos-1] = ch;
					wattron(win,dimColor | A_REVERSE);
                                        mvwprintw(win,vpos,hpos,"%s",field);
                                        wattroff(win,dimColor | A_REVERSE);
					}
			}
		if (moveField) {
			wattron(win,dimColor | A_REVERSE);
			mvwprintw(win,vpos,hpos+cpos," ");
			wattroff(win,dimColor | A_REVERSE);
			cpos = 15;
			switch(fpos) {
				case 0: hpos = IFNAMSIZ+1; field = ip; break;
				case 1: hpos = IFNAMSIZ+IPLEN+5; field = mask; break;
				default: hpos = IFNAMSIZ+IPLEN*2+8; field=gw; break;
				}
			moveField = false;
			}
                // mvwprintw(win,vpos,hpos,"%s ",pathBuf); // clears cursor at rightmost location, if any
                if (cpos < 15) {
                        wattron(win,COLOR_PAIR(CLR_YB) | A_REVERSE);
                        mvwprintw(win,vpos,hpos+cpos,"%c",field[cpos]);
                        wattroff(win,COLOR_PAIR(CLR_YB) | A_REVERSE);
                        }
                else {
                        wattron(win,COLOR_PAIR(CLR_GB) | A_REVERSE);
                        mvwprintw(win,vpos,hpos+cpos," ");
                        wattroff(win,COLOR_PAIR(CLR_GB) | A_REVERSE);
                        }
		if (lastPos != cpos+hpos) {
			wattron(win,dimColor | A_REVERSE);
			mvwprintw(win,vpos,lastPos,"%c",(lastChar)?lastChar:' ');
			wattroff(win,dimColor | A_REVERSE);
			lastPos = cpos+hpos;
			lastChar = field[cpos];
			}
                wrefresh(win);
		} while((ch = getch()) || 1);
DONE:
	return;
	}


void networkSettings(void) {
	int lastFocus;
	int ch;
	int i;
	bool looped = true;
	lastFocus = currentFocus;
	currentFocus = WIN_PROGRESS;
	allWindows(CLEAR);
	refresh();
	// tempPop(); // temporary population of fake values
	populateInterfaces();
	showNetwork(0);

	if (wins[WIN_PROGRESS] == NULL) createWindow(WIN_PROGRESS);
        WINDOW *netwin = wins[WIN_PROGRESS];
	box(netwin,0,0);
	wattron(netwin,COLOR_PAIR(CLR_YB)|  A_BOLD);
	mvwprintw(netwin,1,(COLS-strlen(NETTITLE))/2,NETTITLE);
	wattroff(netwin,COLOR_PAIR(CLR_YB) | A_BOLD);
	wattron(netwin,COLOR_PAIR(CLR_GB));
	mvwprintw(netwin,3,5,"%-*s%*s%*s%*s  %4s",IFNAMSIZ-4,"interface",IPLEN,"address",IPLEN+4,"netmask",IPLEN+3,"gateway","link");
	wattroff(netwin,COLOR_PAIR(CLR_GB));
	int x,y;
	struct ifreq ifr;
	int sock;
	char gw[16], ipBuf[16];
	int t;
	getmaxyx(netwin,y,x);
	showNets(netwin,y,x,false);
	while(looped) {
		ch = getch();
		switch(ch) {
			case KEY_UP: if (currentSelection) {
					currentSelection--;
					if (currentSelection < top) top--;
					showNets(netwin,y,x,false);
					}
				break;
			case KEY_DOWN: if (currentSelection < interfaceCount - 1) {
					currentSelection++;
					if ((currentSelection-top)+7 > y) top++;
					showNets(netwin,y,x,false);
					}
				break;
			case KEY_PPAGE: if (currentSelection) {
					if (currentSelection+7 > y) currentSelection -= (y-7);
					else currentSelection = 0;
					top = currentSelection;
					showNets(netwin,y,x,false);
					}
				break;
			case KEY_NPAGE:
					if (currentSelection < interfaceCount-1) {
						currentSelection += (y-7);
						if (currentSelection >= interfaceCount) currentSelection = interfaceCount-1;
						if (currentSelection+7 > y) top = currentSelection+7-y;
						showNets(netwin,y,x,false);
					}
				break;
			case KEY_F(8): // dhcp
				if (interfaceCount && (!(interfaces[currentSelection].state & INTF_UP))) { // get a lease on this interface
					getLease(netwin);
					showNets(netwin,y,x,true);
					}
				break;
			case KEY_F(6): // edit
				showNetwork(1);
				if (interfaceCount) editField(netwin,(currentSelection-top)+4,&interfaces[currentSelection]);
				showNetwork(0);
				showNets(netwin,y,x,true);
				break;
			case KEY_F(7): // up/down
				if (interfaceCount) {
					if ((sock = socket(AF_INET,SOCK_DGRAM,0)) < 0) break;
        				strncpy(ifr.ifr_name,interfaces[currentSelection].name,IFNAMSIZ);
					if (interfaces[currentSelection].state & INTF_UP) { // bring it down
						releaseLease();
						if (!toggleIntf(sock,&ifr,false)) { debug(INFO,5,"DOWN\n"); close(sock); break; } // bring down (automatically removes gateway)
        					interfaces[currentSelection].state &= ~INTF_UP;
						}
					else {  // bring it up; also activate gateway route if any
						if (!toggleIntf(sock,&ifr,true)) { debug(INFO,5,"UP\n"); close(sock); break; }
						mvwprintw(netwin,1,COLS-14,"WAITLINK    "); wrefresh(netwin);
						t = 0;
						while((t < 1000) && !hasLink(interfaces[currentSelection].name)) { usleep(10000); t++; }  // wait for it to come up (spanning tree, switch, etc.)
						mvwprintw(netwin,1,COLS-14,"            "); wrefresh(netwin);
						if (t == 1000) interfaces[currentSelection].state &= ~HAS_LINK;
						else interfaces[currentSelection].state |= HAS_LINK;
					        inet_ntop(AF_INET,&interfaces[currentSelection].gw,ipBuf,16);
						sprintf(gw,"%15s",(strcmp(ipBuf,"0.0.0.0"))?ipBuf:" ");
        					if (((t = fieldStart(gw)) != 15) && (strcmp(&gw[t],"0.0.0.0"))) { // not blank; also activate gateway (only works in current network settings session)
                					if (!addGateway(sock,&ifr,&gw[t])) { debug(INFO,5,"GATEWAY\n"); close(sock); // check for success here...
								memset(&interfaces[currentSelection].gw,0,sizeof(struct in_addr));
								}
                					}
						interfaces[currentSelection].state |= INTF_UP;
						}
					close(sock);
					showNets(netwin,y,x,true);
					}
				break;
			case KEY_F(1): case 1: looped = false; break;  // done
			}
		}
	wclear(netwin);
	// wborder(netwin,' ',' ',' ',' ',' ',' ',' ',' ');
	wrefresh(netwin);
	currentFocus = lastFocus;
	options |= OPT_NETWORK; // set or clear this depending on network availability
	showFunctionMenu(0);
	}
#endif
