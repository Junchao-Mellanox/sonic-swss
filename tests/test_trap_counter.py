import time
import os
import pytest

from swsscommon import swsscommon


class TestTrapCounter:
    def __init__(self):
        pass

    def test_trap_counter_add(self, dvs):
        app_db = dvs.get_app_db()
        app_db.create_entry('COPP_TABLE', 'group1', {'trap_ids': 'arp,dhcp'})
        
        config_db = dvs.get_config_db()
        group_stats_entry = {"FLEX_COUNTER_STATUS": "enable"}
        config_db.create_entry("FLEX_COUNTER_TABLE", "FLOW_CNT_TRAP", group_stats_entry)

        flex_db = dvs.get_flex_db()
        flex_db.db_connection.hgetall("FLEX_COUNTER_TABLE:" + stat + ":" + oid).items()
        
    def test_test_trap_counter_remove(self):
        pass

    def test_trap_counter_group_update_status(self):
        pass

    def test_trap_counter_group_update_interval(self):
        pass

    def verify_flex_counters_populated(self, map, stat):
        counters_keys = self.counters_db.db_connection.hgetall(map)
        for counter_entry in counters_keys.items():
            name = counter_entry[0]
            oid = counter_entry[1]
            self.wait_for_id_list(stat, name, oid)
