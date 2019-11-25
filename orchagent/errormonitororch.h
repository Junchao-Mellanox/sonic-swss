#pragma once

#include "orch.h"
#include "port.h"
#include "observer.h"
#include "dbconnector.h"
#include "notificationconsumer.h"
#include "selectabletimer.h"
#include "table.h"

#include <string>
#include <map>
#include <vector>

extern "C" {
#include "sai.h"
}


typedef struct ErrorMonitorContext
{
    ErrorMonitorContext(const std::string& name):tx_error_threshold(0), last_tx_error(0), name(name)
    {
    }

    uint64_t tx_error_threshold;
    uint64_t last_tx_error;
    std::string name;

} ErrorMonitorContext;

typedef std::map<std::string, ErrorMonitorContext *> ErrorMonitorMap; // maybe unordered map is a litter bit faster, though there won't be many map entries  

class ErrorMonitorOrch: public Orch, public Observer
{
public:
    ErrorMonitorOrch(swss::DBConnector *db, std::vector<std::string> &tableNames);
    ~ErrorMonitorOrch() override;
    void doTask(Consumer &consumer) override;
    void doTask(swss::NotificationConsumer &consumer) override;
    void doTask(swss::SelectableTimer &timer) override;

    void update(SubjectType, void *) override; // override Observer

private:
    void onTxErrorThresholdChanged(const std::string &key, const std::string &value);
    void onTxErrorStatusChanged(const std::string &key, const std::string &value);
    void onPollIntervalChanged(const std::string &value);

    void onClear();
    void onClear(const std::string &port_name);
    void onClear(ErrorMonitorContext &);

    void onEnable(ErrorMonitorContext &);
    void onDisable(ErrorMonitorContext &);

    void onRemove(const std::string &port_name);

    void removeAllStatus();

    // get or create a ErrorMonitorContext instance 
    ErrorMonitorContext *getErrorMonitorContext(const std::string& port_name);
    ErrorMonitorContext *getErrorMonitorContext(const std::string &key, const std::string &sep, bool forceCreate=true);
    ErrorMonitorContext *createErrorMonitorContext(const std::string &port_name);

    std::shared_ptr<swss::DBConnector> m_stateDb;
    std::unique_ptr<swss::Table> m_errorStatusTable;
    std::unique_ptr<swss::Table> m_configTable;

    swss::SelectableTimer* m_pollTimer = nullptr;

    ErrorMonitorMap m_contextMap; // cache context that user configured
    ErrorMonitorMap m_pollMap; // cache context that user enabled
};
