#include "errormonitororch.h"

#include "logger.h"
#include "schema.h"
#include "converter.h"
#include "portsorch.h"
#include "notifier.h"
#include "timer.h"
#include "tokenize.h"

#include <limits>
#include <sys/time.h>
#include <inttypes.h>

#define DEFAULT_POLL_INTERVAL 0
#define CLEAR_NOTIFIER_NAME "ERROR_MONITOR_CLEAR_NOTIFIER"
#define ERROR_MONITOR_POLL_TIMER_NAME "ERROR_MONITOR_POLL_TIMER"
#define CLEAR_REQUEST "CLEAR_ERROR_MONITOR"
#define CLEAR_ALL "ALL"

const std::vector<sai_port_stat_t> portStatIds = 
{
    SAI_PORT_STAT_IF_OUT_ERRORS, // currently only tx error
};

extern PortsOrch *gPortsOrch;
extern sai_port_api_t *sai_port_api;

ErrorMonitorOrch::ErrorMonitorOrch(swss::DBConnector *db, std::vector<std::string> &tableNames):
        Orch(db, tableNames)
{
    SWSS_LOG_ENTER();

    m_stateDb = std::shared_ptr<DBConnector>(new DBConnector(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0));
    m_errorStatusTable = std::unique_ptr<Table>(new Table(m_stateDb.get(), STATE_ERROR_MONITOR_TABLE_NAME));
    m_configTable = std::unique_ptr<Table>(new Table(db, CFG_ERROR_MONITOR_THRESHOLD_TABLE_NAME));

    // initialize notify consumer to listen CLEAR_ERROR_MONITOR channel
    auto clearNotifyConsumer = new swss::NotificationConsumer(
            m_stateDb.get(),
            CLEAR_REQUEST);
    auto clearNotifier = new Notifier(clearNotifyConsumer, this, CLEAR_NOTIFIER_NAME);
    Orch::addExecutor(clearNotifier);

    // initialize poll timer
    auto intervT = timespec { .tv_sec = DEFAULT_POLL_INTERVAL , .tv_nsec = 0 };
    m_pollTimer = new SelectableTimer(intervT);
    auto executorT = new ExecutableTimer(m_pollTimer, this, ERROR_MONITOR_POLL_TIMER_NAME);
    Orch::addExecutor(executorT);

    // observe port change event
    gPortsOrch->attach(this);

    // remove status DB
    removeAllStatus();
}

ErrorMonitorOrch::~ErrorMonitorOrch()
{
    SWSS_LOG_ENTER();

    for (auto &kv : m_contextMap)
    {
        m_errorStatusTable->del(kv.second->name); // not sure if "del" throw exception or not...better add try catch?
        m_configTable->del(kv.second->name);
        delete kv.second;
    }

    m_errorStatusTable->flush(); 
    m_configTable->flush();

    m_contextMap.clear();
    m_pollMap.clear();

    // shall we free m_pollTimer and all consumer here? 

    gPortsOrch->detach(this); // who can make sure gPortsOrch lives longer than ErrorMonitorOrch
}

void ErrorMonitorOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    
    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        std::string key =  kfvKey(t);
        std::string op = kfvOp(t);
        auto data = kfvFieldsValues(t);

        if (op == SET_COMMAND)
        {
            for (auto &valuePair:data)
            {
                const auto &field = fvField(valuePair);
                const auto &value = fvValue(valuePair);

                if (field == ERROR_MONITOR_TX_ERROR_THRESHOLD_FIELD)
                {
                    onTxErrorThresholdChanged(key, value);
                }
                else if (field == ERROR_MONITOR_TX_ERROR_STATUS_FIELD)
                {
                    onTxErrorStatusChanged(key, value);
                }
                else if (field == ERROR_MONITOR_POLL_INTERVAL_FIELD)
                {
                    onPollIntervalChanged(value);
                }
                else
                {
                    SWSS_LOG_NOTICE("Unsupported field %s", field.c_str());
                }
            }
        }

        consumer.m_toSync.erase(it++);
    }
}

void ErrorMonitorOrch::doTask(swss::NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);
    if (op == ERROR_MONITOR_TX_STATUS_FIELD)
    {
        if (data == CLEAR_ALL)
        {
            onClear();
        }
        else
        {
            onClear(data);
        }

        m_errorStatusTable->flush();
    }
    else
    {
        SWSS_LOG_NOTICE("Unsupported clear operation %s", op.c_str());
    }
}

void ErrorMonitorOrch::doTask(swss::SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    Port port;
    sai_status_t status;
    uint64_t tx_error;
    std::string tx_status;

    for (auto &kv : m_pollMap)
    {
        ErrorMonitorContext *context = kv.second;
        if (!gPortsOrch->getPort(context->name, port))
        {
            SWSS_LOG_ERROR("Failed to get port object : %s", context->name.c_str());
            continue; // should not happen...
        }

        std::vector<uint64_t> portStats(1);
        status = sai_port_api->get_port_stats(
                port.m_port_id,
                static_cast<uint32_t>(portStatIds.size()),
                (const sai_stat_id_t *)portStatIds.data(),
                portStats.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get stats of port %s: %d", context->name.c_str(), status);
            continue;
        }

        tx_error = portStats[0];
        if (tx_error >= context->last_tx_error)
        {
            
            if (tx_error - context->last_tx_error > context->tx_error_threshold)
            {
                tx_status = "Not OK";
            }
            else
            {
                tx_status = "OK";
            }

            m_errorStatusTable->hset(context->name, ERROR_MONITOR_TX_STATUS_FIELD, tx_status);
            m_errorStatusTable->hset(context->name, ERROR_MONITOR_TX_ERROR_FIELD, std::to_string(tx_error - context->last_tx_error));
        }
        else
        {
            // maybe someone clear the counter just now, ignore this round
        }

        context->last_tx_error = tx_error;
    }

    m_errorStatusTable->flush();
}

void ErrorMonitorOrch::update(SubjectType subject, void *cntx)
{
    SWSS_LOG_ENTER();

    if (subject == SUBJECT_TYPE_PORT_CHANGE)
    {
        PortUpdate *update = static_cast<PortUpdate *>(cntx);
        if (update->port.m_type != Port::PHY)
        {
            return;
        }

        // we only care port remove event
        if (!update->add)
        {
            onRemove(update->port.m_alias);
            m_errorStatusTable->flush();
            m_configTable->flush();
        }
    }
}

void ErrorMonitorOrch::onTxErrorThresholdChanged(const std::string &key, const std::string &value)
{
    SWSS_LOG_ENTER();

    uint64_t threshold = 0;
    try
    {
        threshold = swss::to_uint<uint64_t>(value);
    }
    catch(const std::invalid_argument &e)
    {
        SWSS_LOG_ERROR("Invalid tx error threshold value: %s", value.c_str());
        return;
    }

    ErrorMonitorContext *context = getErrorMonitorContext(key, CONFIGDB_KEY_SEPARATOR);

    if (context)
    {
        context->tx_error_threshold = threshold;
    }
}

void ErrorMonitorOrch::onTxErrorStatusChanged(const std::string &key, const std::string &value)
{
    SWSS_LOG_ENTER();

    ErrorMonitorContext *context = getErrorMonitorContext(key, CONFIGDB_KEY_SEPARATOR);

    if (context)
    {
        if (value == "enable")
        {
            onEnable(*context);
            m_errorStatusTable->flush();
        }
        else if (value == "disable")
        {
            onDisable(*context);
            m_errorStatusTable->flush();
        }
        else
        {
            SWSS_LOG_ERROR("Invalid tx error status value: %s", value.c_str());
            // still treat it as a disable?
        }
    }
}

void ErrorMonitorOrch::onPollIntervalChanged(const std::string &value)
{
    SWSS_LOG_ENTER();

    uint32_t interval = 0;
    try
    {
        interval = swss::to_uint<uint32_t>(value);
    }
    catch(const std::invalid_argument &e)
    {
        SWSS_LOG_ERROR("Invalid tx error value: %s", value.c_str());
        return;
    }

    if (interval == 0)
    {
        m_pollTimer->stop(); // if the timer has not been started, this would generate an error log that should be ignored
    }
    else
    {
        auto intervT = timespec { .tv_sec = interval , .tv_nsec = 0 };
        m_pollTimer->setInterval(intervT);
        m_pollTimer->reset();
    }
    
}

void ErrorMonitorOrch::onClear()
{
    SWSS_LOG_ENTER();

    for(auto &kv : m_contextMap)
    {
        onClear(*kv.second);
    }
}

void ErrorMonitorOrch::onClear(const std::string &port_name)
{
    SWSS_LOG_ENTER();

    ErrorMonitorContext *context = getErrorMonitorContext(port_name);
    if (context)
    {
        onClear(*context);
    }
}

void ErrorMonitorOrch::onClear(ErrorMonitorContext &context)
{
    SWSS_LOG_ENTER();

    context.last_tx_error = 0;
    std::string dummy;
    if (m_errorStatusTable->hget(context.name, ERROR_MONITOR_TX_STATUS_FIELD, dummy))
    {
        m_errorStatusTable->hset(context.name, ERROR_MONITOR_TX_STATUS_FIELD, "OK");
        m_errorStatusTable->hset(context.name, ERROR_MONITOR_TX_ERROR_FIELD, "0");
    }
}

void ErrorMonitorOrch::onEnable(ErrorMonitorContext &context)
{
    SWSS_LOG_ENTER();

    auto iter = m_pollMap.find(context.name);
    if (iter == m_pollMap.end())
    {
        m_errorStatusTable->hset(context.name, ERROR_MONITOR_TX_STATUS_FIELD, "OK");
        m_errorStatusTable->hset(context.name, ERROR_MONITOR_TX_ERROR_FIELD, "0");
        m_pollMap.emplace(context.name, &context);
    }
}

void ErrorMonitorOrch::onDisable(ErrorMonitorContext &context)
{
    SWSS_LOG_ENTER();

    auto iter = m_pollMap.find(context.name);
    if (iter != m_pollMap.end())
    {
        m_errorStatusTable->del(context.name);
        m_pollMap.erase(iter);
    }
}

void ErrorMonitorOrch::onRemove(const std::string &port_name)
{
    SWSS_LOG_ENTER();

    auto iter = m_contextMap.find(port_name);
    if (iter != m_contextMap.end())
    {
        onDisable(*iter->second);
        m_configTable->del(port_name);
        delete iter->second;
        m_contextMap.erase(iter);
    }
}

void ErrorMonitorOrch::removeAllStatus()
{
    std::vector<std::string> keys;
    m_errorStatusTable->getKeys(keys);
    for (const auto& key : keys)
    {
        m_errorStatusTable->del(key);
    }

    m_errorStatusTable->flush();
}

ErrorMonitorContext *ErrorMonitorOrch::getErrorMonitorContext(const std::string& port_name)
{
    SWSS_LOG_ENTER();

    auto iter = m_contextMap.find(port_name);
    return iter != m_contextMap.end() ? iter->second : nullptr;
}

ErrorMonitorContext *ErrorMonitorOrch::getErrorMonitorContext(const std::string &key, const std::string &sep, bool forceCreate)
{
    SWSS_LOG_ENTER();

    std::vector<std::string> keys = swss::tokenize(key, sep);
    if (keys.size() != 2)
    {
        SWSS_LOG_ERROR("Invalid key: %s", key.c_str());
        return nullptr;
    }

    std::string port_name = keys[1];
    Port port;
    if (!gPortsOrch->getPort(port_name, port))
    {
        SWSS_LOG_ERROR("Retrive port failed with port name: %s", port_name.c_str());
        return nullptr;
    }

    if (port.m_type != Port::PHY)
    {
        SWSS_LOG_ERROR("Invalid port type for port: %s", port_name.c_str());
        return nullptr;
    }

    auto iter = m_contextMap.find(port_name);
    if (iter == m_contextMap.end())
    {
        return forceCreate ? createErrorMonitorContext(port_name) : nullptr;
    }
    else
    {
        return iter->second;
    }
}

ErrorMonitorContext *ErrorMonitorOrch::createErrorMonitorContext(const std::string &port_name)
{
    SWSS_LOG_ENTER();

    auto ret = m_contextMap.emplace(port_name, new ErrorMonitorContext(port_name));
    if (ret.second)
    {
        return ret.first->second;
    }

    return nullptr; // would never happend since createErrorMonitorContext is only called in getErrorMonitorContext for now
}
