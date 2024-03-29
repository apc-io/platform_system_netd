/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <resolv_netid.h>

#define LOG_TAG "SecondaryTablController"
#include <cutils/log.h>
#include <cutils/properties.h>
#include <logwrap/logwrap.h>

#include "ResponseCode.h"
#include "NetdConstants.h"
#include "SecondaryTableController.h"

const char* SecondaryTableController::LOCAL_MANGLE_OUTPUT = "st_mangle_OUTPUT";
const char* SecondaryTableController::LOCAL_MANGLE_EXEMPT = "st_mangle_EXEMPT";
const char* SecondaryTableController::LOCAL_MANGLE_IFACE_FORMAT = "st_mangle_%s_OUTPUT";
const char* SecondaryTableController::LOCAL_NAT_POSTROUTING = "st_nat_POSTROUTING";
const char* SecondaryTableController::LOCAL_FILTER_OUTPUT = "st_filter_OUTPUT";

SecondaryTableController::SecondaryTableController(NetworkController* controller) :
        mNetCtrl(controller) {
}

SecondaryTableController::~SecondaryTableController() {
}

int SecondaryTableController::setupIptablesHooks() {
    int res = execIptables(V4V6,
            "-t",
            "mangle",
            "-F",
            LOCAL_MANGLE_OUTPUT,
            NULL);
    res |= execIptables(V4V6,
            "-t",
            "mangle",
            "-F",
            LOCAL_MANGLE_EXEMPT,
            NULL);
    // rule for skipping anything marked with the PROTECT_MARK
    char protect_mark_str[11];
    snprintf(protect_mark_str, sizeof(protect_mark_str), "%d", PROTECT_MARK);
    res |= execIptables(V4V6,
            "-t",
            "mangle",
            "-A",
            LOCAL_MANGLE_OUTPUT,
            "-m",
            "mark",
            "--mark",
            protect_mark_str,
            "-j",
            "RETURN",
            NULL);

    // protect the legacy VPN daemons from routes.
    // TODO: Remove this when legacy VPN's are removed.
    res |= execIptables(V4V6,
            "-t",
            "mangle",
            "-A",
            LOCAL_MANGLE_OUTPUT,
            "-m",
            "owner",
            "--uid-owner",
            "vpn",
            "-j",
            "RETURN",
            NULL);
    return res;
}

int SecondaryTableController::addRoute(SocketClient *cli, char *iface, char *dest, int prefix,
        char *gateway) {
    return modifyRoute(cli, ADD, iface, dest, prefix, gateway, mNetCtrl->getNetworkId(iface));
}

int SecondaryTableController::modifyRoute(SocketClient *cli, const char *action, char *iface,
        char *dest, int prefix, char *gateway, unsigned netId) {
    char dest_str[44]; // enough to store an IPv6 address + 3 character bitmask
    char tableIndex_str[11];
    int ret;

    //  IP tool doesn't like "::" - the equiv of 0.0.0.0 that it accepts for ipv4
    snprintf(dest_str, sizeof(dest_str), "%s/%d", dest, prefix);
    snprintf(tableIndex_str, sizeof(tableIndex_str), "%u", netId + BASE_TABLE_NUMBER);

    if (strcmp("::", gateway) == 0) {
        const char *cmd[] = {
                IP_PATH,
                "route",
                action,
                dest_str,
                "dev",
                iface,
                "table",
                tableIndex_str
        };
        ret = runCmd(ARRAY_SIZE(cmd), cmd);
    } else {
        const char *cmd[] = {
                IP_PATH,
                "route",
                action,
                dest_str,
                "via",
                gateway,
                "dev",
                iface,
                "table",
                tableIndex_str
        };
        ret = runCmd(ARRAY_SIZE(cmd), cmd);
    }

    if (ret) {
        ALOGE("ip route %s failed: %s route %s %s/%d via %s dev %s table %u", action,
                IP_PATH, action, dest, prefix, gateway, iface, netId + BASE_TABLE_NUMBER);
        errno = ENODEV;
        cli->sendMsg(ResponseCode::OperationFailed, "ip route modification failed", true);
        return -1;
    }

    modifyRuleCount(netId, action);
    cli->sendMsg(ResponseCode::CommandOkay, "Route modified", false);
    return 0;
}

void SecondaryTableController::modifyRuleCount(unsigned netId, const char *action) {
    if (strcmp(action, ADD) == 0) {
        if (mNetIdRuleCount.count(netId) == 0)
            mNetIdRuleCount[netId] = 0;
        mNetIdRuleCount[netId]++;
    } else {
        if (mNetIdRuleCount.count(netId) > 0) {
            if (--mNetIdRuleCount[netId] < 1) {
                mNetIdRuleCount.erase(mNetIdRuleCount.find(netId));
            }
        }
    }
}

const char *SecondaryTableController::getVersion(const char *addr) {
    if (strchr(addr, ':') != NULL) {
        return "-6";
    } else {
        return "-4";
    }
}

IptablesTarget SecondaryTableController::getIptablesTarget(const char *addr) {
    if (strchr(addr, ':') != NULL) {
        return V6;
    } else {
        return V4;
    }
}

int SecondaryTableController::removeRoute(SocketClient *cli, char *iface, char *dest, int prefix,
        char *gateway) {
    return modifyRoute(cli, DEL, iface, dest, prefix, gateway, mNetCtrl->getNetworkId(iface));
}

int SecondaryTableController::modifyFromRule(unsigned netId, const char *action,
        const char *addr) {
    char tableIndex_str[11];

    snprintf(tableIndex_str, sizeof(tableIndex_str), "%u", netId + BASE_TABLE_NUMBER);
    const char *cmd[] = {
            IP_PATH,
            getVersion(addr),
            "rule",
            action,
            "from",
            addr,
            "table",
            tableIndex_str
    };
    if (runCmd(ARRAY_SIZE(cmd), cmd)) {
        return -1;
    }

    modifyRuleCount(netId, action);
    return 0;
}

int SecondaryTableController::modifyLocalRoute(unsigned netId, const char *action,
        const char *iface, const char *addr) {
    char tableIndex_str[11];

    modifyRuleCount(netId, action); // some del's will fail as the iface is already gone.
    snprintf(tableIndex_str, sizeof(tableIndex_str), "%u", netId + BASE_TABLE_NUMBER);
    const char *cmd[] = {
            IP_PATH,
            "route",
            action,
            addr,
            "dev",
            iface,
            "table",
            tableIndex_str
    };

    return runCmd(ARRAY_SIZE(cmd), cmd);
}
int SecondaryTableController::addFwmarkRule(const char *iface) {
    return setFwmarkRule(iface, true);
}

int SecondaryTableController::removeFwmarkRule(const char *iface) {
    return setFwmarkRule(iface, false);
}

int SecondaryTableController::setFwmarkRule(const char *iface, bool add) {
    unsigned netId = mNetCtrl->getNetworkId(iface);

    // Fail fast if any rules already exist for this interface
    if (mNetIdRuleCount.count(netId) > 0) {
        errno = EBUSY;
        return -1;
    }

    int ret;
    char mark_str[11];
    snprintf(mark_str, sizeof(mark_str), "%u", netId + BASE_TABLE_NUMBER);
    //add the catch all route to the tun. Route rules will make sure the right packets hit the table
    const char *route_cmd[] = {
        IP_PATH,
        "route",
        add ? "add" : "del",
        "default",
        "dev",
        iface,
        "table",
        mark_str
    };
    ret = runCmd(ARRAY_SIZE(route_cmd), route_cmd);

    const char *fwmark_cmd[] = {
        IP_PATH,
        "rule",
        add ? "add" : "del",
        "prio",
        RULE_PRIO,
        "fwmark",
        mark_str,
        "table",
        mark_str
    };
    ret = runCmd(ARRAY_SIZE(fwmark_cmd), fwmark_cmd);
    if (ret) return ret;

    //add rules for v6
    const char *route6_cmd[] = {
        IP_PATH,
        "-6",
        "route",
        add ? "add" : "del",
        "default",
        "dev",
        iface,
        "table",
        mark_str
    };
    ret = runCmd(ARRAY_SIZE(route6_cmd), route6_cmd);

    const char *fwmark6_cmd[] = {
        IP_PATH,
        "-6",
        "rule",
        add ? "add" : "del",
        "prio",
        RULE_PRIO,
        "fwmark",
        mark_str,
        "table",
        mark_str
    };
    ret = runCmd(ARRAY_SIZE(fwmark6_cmd), fwmark6_cmd);


    if (ret) return ret;

    //create the route rule chain
    char chain_str[IFNAMSIZ + 18];
    snprintf(chain_str, sizeof(chain_str), LOCAL_MANGLE_IFACE_FORMAT, iface);
    //code split due to ordering requirements
    if (add) {
        ret = execIptables(V4V6,
                "-t",
                "mangle",
                "-N",
                chain_str,
                NULL);
        //set up the rule for sending premarked packets to the VPN chain
        //Insert these at the top of the chain so they trigger before any UID rules
        ret |= execIptables(V4V6,
                "-t",
                "mangle",
                "-I",
                LOCAL_MANGLE_OUTPUT,
                "3",
                "-m",
                "mark",
                "--mark",
                mark_str,
                "-g",
                chain_str,
                NULL);
        //add a rule to clear the mark in the VPN chain
        //packets marked with SO_MARK already have the iface's mark set but unless they match a
        //route they should hit the network instead of the VPN
        ret |= execIptables(V4V6,
                "-t",
                "mangle",
                "-A",
                chain_str,
                "-j",
                "MARK",
                "--set-mark",
                "0",
                NULL);

    } else {
        ret = execIptables(V4V6,
                "-t",
                "mangle",
                "-D",
                LOCAL_MANGLE_OUTPUT,
                "-m",
                "mark",
                "--mark",
                mark_str,
                "-g",
                chain_str,
                NULL);

        //clear and delete the chain
        ret |= execIptables(V4V6,
                "-t",
                "mangle",
                "-F",
                chain_str,
                NULL);

        ret |= execIptables(V4V6,
                "-t",
                "mangle",
                "-X",
                chain_str,
                NULL);
    }

    //set up the needed source IP rewriting
    //NOTE: Without ipv6 NAT in the kernel <3.7 only support V4 NAT
    ret = execIptables(V4,
            "-t",
            "nat",
            add ? "-A" : "-D",
            LOCAL_NAT_POSTROUTING,
            "-o",
            iface,
            "-m",
            "mark",
            "--mark",
            mark_str,
            "-j",
            "MASQUERADE",
            NULL);

    if (ret) return ret;

    //try and set up for ipv6. ipv6 nat came in the kernel only in 3.7, so this can fail
    ret = execIptables(V6,
            "-t",
            "nat",
            add ? "-A" : "-D",
            LOCAL_NAT_POSTROUTING,
            "-o",
            iface,
            "-m",
            "mark",
            "--mark",
            mark_str,
            "-j",
            "MASQUERADE",
            NULL);
    if (ret) {
        //Without V6 NAT we can't do V6 over VPNs.
        ret = execIptables(V6,
                "-t",
                "filter",
                add ? "-A" : "-D",
                LOCAL_FILTER_OUTPUT,
                "-m",
                "mark",
                "--mark",
                mark_str,
                "-j",
                "REJECT",
                NULL);
    }
    return ret;

}

int SecondaryTableController::addFwmarkRoute(const char* iface, const char *dest, int prefix) {
    return setFwmarkRoute(iface, dest, prefix, true);
}

int SecondaryTableController::removeFwmarkRoute(const char* iface, const char *dest, int prefix) {
    return setFwmarkRoute(iface, dest, prefix, false);
}

int SecondaryTableController::setFwmarkRoute(const char* iface, const char *dest, int prefix,
                                             bool add) {
    unsigned netId = mNetCtrl->getNetworkId(iface);
    char mark_str[11] = {0};
    char chain_str[IFNAMSIZ + 18];
    char dest_str[44]; // enough to store an IPv6 address + 3 character bitmask

    snprintf(mark_str, sizeof(mark_str), "%u", netId + BASE_TABLE_NUMBER);
    snprintf(chain_str, sizeof(chain_str), LOCAL_MANGLE_IFACE_FORMAT, iface);
    snprintf(dest_str, sizeof(dest_str), "%s/%d", dest, prefix);
    return execIptables(getIptablesTarget(dest),
            "-t",
            "mangle",
            add ? "-A" : "-D",
            chain_str,
            "-d",
            dest_str,
            "-j",
            "MARK",
            "--set-mark",
            mark_str,
            NULL);
}

int SecondaryTableController::addUidRule(const char *iface, int uid_start, int uid_end) {
    return setUidRule(iface, uid_start, uid_end, true);
}

int SecondaryTableController::removeUidRule(const char *iface, int uid_start, int uid_end) {
    return setUidRule(iface, uid_start, uid_end, false);
}

int SecondaryTableController::setUidRule(const char *iface, int uid_start, int uid_end, bool add) {
    unsigned netId = mNetCtrl->getNetworkId(iface);
    if (!mNetCtrl->setNetworkForUidRange(uid_start, uid_end, add ? netId : 0, false)) {
        errno = EINVAL;
        return -1;
    }

    char uid_str[24] = {0};
    char chain_str[IFNAMSIZ + 18];
    snprintf(uid_str, sizeof(uid_str), "%d-%d", uid_start, uid_end);
    snprintf(chain_str, sizeof(chain_str), LOCAL_MANGLE_IFACE_FORMAT, iface);
    return execIptables(V4V6,
            "-t",
            "mangle",
            add ? "-A" : "-D",
            LOCAL_MANGLE_OUTPUT,
            "-m",
            "owner",
            "--uid-owner",
            uid_str,
            "-g",
            chain_str,
            NULL);
}

int SecondaryTableController::addHostExemption(const char *host) {
    return setHostExemption(host, true);
}

int SecondaryTableController::removeHostExemption(const char *host) {
    return setHostExemption(host, false);
}

int SecondaryTableController::setHostExemption(const char *host, bool add) {
    IptablesTarget target = !strcmp(getVersion(host), "-4") ? V4 : V6;
    char protect_mark_str[11];
    snprintf(protect_mark_str, sizeof(protect_mark_str), "%d", PROTECT_MARK);
    int ret = execIptables(target,
            "-t",
            "mangle",
            add ? "-A" : "-D",
            LOCAL_MANGLE_EXEMPT,
            "-d",
            host,
            "-j",
            "MARK",
            "--set-mark",
            protect_mark_str,
            NULL);
    const char *cmd[] = {
        IP_PATH,
        getVersion(host),
        "rule",
        add ? "add" : "del",
        "prio",
        EXEMPT_PRIO,
        "to",
        host,
        "table",
        "main"
    };
    ret |= runCmd(ARRAY_SIZE(cmd), cmd);
    return ret;
}

void SecondaryTableController::getUidMark(SocketClient *cli, int uid) {
    unsigned netId = mNetCtrl->getNetwork(uid, NETID_UNSET, NetworkController::PID_UNSPECIFIED,
            false);
    char mark_str[11];
    snprintf(mark_str, sizeof(mark_str), "%u", netId + BASE_TABLE_NUMBER);
    cli->sendMsg(ResponseCode::GetMarkResult, mark_str, false);
}

void SecondaryTableController::getProtectMark(SocketClient *cli) {
    char protect_mark_str[11];
    snprintf(protect_mark_str, sizeof(protect_mark_str), "%d", PROTECT_MARK);
    cli->sendMsg(ResponseCode::GetMarkResult, protect_mark_str, false);
}

int SecondaryTableController::runCmd(int argc, const char **argv) {
    int ret = 0;

    ret = android_fork_execvp(argc, (char **)argv, NULL, false, false);
    return ret;
}
