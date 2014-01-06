/* **********************************************************
 * Copyright 2007 VMware, Inc.  All rights reserved. 
 * **********************************************************
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 */

/*
 * osNames.h --
 *
 *    This file contains strings often found in various guest OS names.
 */

#ifndef _OS_NAMES_H_
#define _OS_NAMES_H_

/* Linux */
#define STR_OS_ANNVIX "Annvix" 
#define STR_OS_ARCH "Arch" 
#define STR_OS_ARKLINUX "Arklinux" 
#define STR_OS_AUROX "Aurox" 
#define STR_OS_BLACKCAT "BlackCat" 
#define STR_OS_COBALT "Cobalt" 
#define STR_OS_CONECTIVA "Conectiva" 
#define STR_OS_DEBIAN "Debian" 
#define STR_OS_FEDORA "Fedora" 
#define STR_OS_GENTOO "Gentoo" 
#define STR_OS_IMMUNIX "Immunix" 
#define STR_OS_LINUX "linux" 
#define STR_OS_LINUX_FROM_SCRATCH "Linux-From-Scratch" 
#define STR_OS_LINUX_FULL "Other Linux"
#define STR_OS_LINUX_PPC "Linux-PPC" 
#define STR_OS_MANDRAKE "mandrake" 
#define STR_OS_MANDRAKE_FULL "Mandrake Linux"   
#define STR_OS_MANDRAVIA "Mandriva"    
#define STR_OS_MKLINUX "MkLinux"    
#define STR_OS_NOVELL "nld9"    
#define STR_OS_NOVELL_FULL "Novell Linux Desktop 9" 
#define STR_OS_OTHER "otherlinux"    
#define STR_OS_OTHER_24 "other24xlinux"    
#define STR_OS_OTHER_24_FULL "Other Linux 2.4.x kernel" 
#define STR_OS_OTHER_26 "other26xlinux"    
#define STR_OS_OTHER_26_FULL "Other Linux 2.6.x kernel" 
#define STR_OS_OTHER_FULL "Other Linux"   
#define STR_OS_PLD "PLD"    
#define STR_OS_RED_HAT  "redhat"   
#define STR_OS_RED_HAT_EN "rhel"    
#define STR_OS_RED_HAT_EN_2 "rhel2"    
#define STR_OS_RED_HAT_EN_2_FULL "Red Hat Enterprise Linux 2"
#define STR_OS_RED_HAT_EN_3 "rhel3"    
#define STR_OS_RED_HAT_EN_3_FULL "Red Hat Enterprise Linux 3"
#define STR_OS_RED_HAT_EN_4 "rhel4"    
#define STR_OS_RED_HAT_EN_4_FULL "Red Hat Enterprise Linux 4"
#define STR_OS_RED_HAT_FULL "Red Hat Linux"  
#define STR_OS_SLACKWARE "Slackware"    
#define STR_OS_SMESERVER "SMEServer"    
#define STR_OS_SUN_DESK "sjds"    
#define STR_OS_SUN_DESK_FULL "Sun Java Desktop System" 
#define STR_OS_SUSE "suse"    
#define STR_OS_SUSE_EN "sles"    
#define STR_OS_SUSE_EN_FULL "SUSE Linux Enterprise Server" 
#define STR_OS_SUSE_FULL "SUSE Linux"   
#define STR_OS_TINYSOFA "Tiny Sofa"   
#define STR_OS_TURBO "turbolinux"    
#define STR_OS_TURBO_FULL "Turbolinux"    
#define STR_OS_UBUNTU "Ubuntu" 
#define STR_OS_ULTRAPENGUIN "UltraPenguin" 
#define STR_OS_UNITEDLINUX "UnitedLinux" 
#define STR_OS_VALINUX "VALinux" 
#define STR_OS_YELLOW_DOG "Yellow Dog"

/* Windows */
#define STR_OS_WIN_31 "win31"
#define STR_OS_WIN_31_FULL "Windows 3.1"
#define STR_OS_WIN_95 "win95"
#define STR_OS_WIN_95_FULL "Windows 95"
#define STR_OS_WIN_98 "win98"
#define STR_OS_WIN_98_FULL "Windows 98"
#define STR_OS_WIN_ME "winMe"
#define STR_OS_WIN_ME_FULL "Windows Me"
#define STR_OS_WIN_NT "winNT"
#define STR_OS_WIN_NT_FULL "Windows NT"
#define STR_OS_WIN_2000_PRO "win2000Pro"
#define STR_OS_WIN_2000_PRO_FULL "Windows 2000 Professional"
#define STR_OS_WIN_2000_SERV   "win2000Serv"
#define STR_OS_WIN_2000_SERV_FULL "Windows 2000 Server"
#define STR_OS_WIN_2000_ADV_SERV "win2000AdvServ"
#define STR_OS_WIN_2000_ADV_SERV_FULL "Windows 2000 Advanced Server"
#define STR_OS_WIN_2000_DATACENT_SERV "win2000DataCentServ"
#define STR_OS_WIN_2000_DATACENT_SERV_FULL "Windows 2000 Data Center Server"
#define STR_OS_WIN_XP_HOME "winXPHome"
#define STR_OS_WIN_XP_HOME_FULL "Windows XP Home Edition"
#define STR_OS_WIN_XP_PRO   "winXPPro"
#define STR_OS_WIN_XP_PRO_FULL "Windows XP Professional"
#define STR_OS_WIN_XP_PRO_X64   "winXPPro-64"
#define STR_OS_WIN_XP_PRO_X64_FULL "Windows XP Professional x64 Edition"
#define STR_OS_WIN_NET_WEB  "winNetWeb" 
#define STR_OS_WIN_NET_WEB_FULL "Windows Server 2003 Web Edition"
#define STR_OS_WIN_NET_ST "winNetStandard"
#define STR_OS_WIN_NET_ST_FULL "Windows Server 2003 Standard Edition"
#define STR_OS_WIN_NET_EN "winNetEnterprise"
#define STR_OS_WIN_NET_EN_FULL "Windows Server 2003 Enterprise Edition"
#define STR_OS_WIN_NET_BUS "winNetBusiness"
#define STR_OS_WIN_NET_BUS_FULL "Windows Server 2003 Small Business"
#define STR_OS_WIN_NET_COMPCLUSTER "winNetComputeCluster"
#define STR_OS_WIN_NET_COMPCLUSTER_FULL "Windows Server 2003 Compute Cluster Edition"
#define STR_OS_WIN_NET_STORAGESERVER "winNetStorageSvr"
#define STR_OS_WIN_NET_STORAGESERVER_FULL "Windows Storage Server 2003"
#define STR_OS_WIN_LONG "longhorn"
#define STR_OS_WIN_LONG_FULL "Longhorn (experimental)"
#define STR_OS_WIN_VISTA "winVista"
#define STR_OS_WIN_VISTA_FULL "Windows Vista"
#define STR_OS_WIN_VISTA_X64 "winVista-64"
#define STR_OS_WIN_VISTA_X64_FULL "Windows Vista x64 Edition"

/* FreeBSD */
#define STR_OS_FREEBSD "FreeBSD"

/* Solaris */
#define STR_OS_SOLARIS "Solaris"

/* All */
#define STR_OS_64BIT_SUFFIX "-64"

#endif // _OS_NAMES_H_
