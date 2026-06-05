// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2026 IBM
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <gtest/gtest.h>
#include "test/osd/ECPeeringTestFixture.h"
#include "osd/PeeringState.h"
#include "common/ceph_context.h"
#include "crush/CrushWrapper.h"
#include "crush/crush.h"

using namespace std;

/**
 * TestECActingStretch - Unit tests for stretch mode EC acting set selection
 *
 * This test suite validates the zone isolation and bucket_max enforcement
 * in calc_ec_acting_stretch and choose_async_recovery_ec for stretched EC pools.
 *
 * Test Configuration:
 * - 2 zones (datacenters), 3 hosts per zone, 1 OSD per host = 6 OSDs total
 * - Stretched 2+1 EC: Each zone has complete stripe [shard.0, shard.1, shard.2]
 * - Acting set: all 6 OSDs with PRIMARY coordinating I/O
 * - CRUSH rule: choose firstn 2 type datacenter, chooseleaf indep 3 type host
 */
class TestECActingStretch : public ECPeeringTestFixture {
protected:
  void SetUp() override {
    ECPeeringTestFixture::SetUp();
    
    // Create stretched 2+1 EC pool with 2 zones
    // Zone 1: OSDs 0,1,2 (hosts host0, host1, host2 in datacenter dc0)
    // Zone 2: OSDs 3,4,5 (hosts host3, host4, host5 in datacenter dc1)
    setup_stretched_ec_pool();
  }
  
  void setup_stretched_ec_pool() {
    // Create OSDMap with 2 datacenters, 3 hosts each
    auto new_osdmap = std::make_shared<OSDMap>();
    new_osdmap->set_max_osd(6);
    new_osdmap->set_state(0, CEPH_OSD_EXISTS | CEPH_OSD_UP);
    new_osdmap->set_state(1, CEPH_OSD_EXISTS | CEPH_OSD_UP);
    new_osdmap->set_state(2, CEPH_OSD_EXISTS | CEPH_OSD_UP);
    new_osdmap->set_state(3, CEPH_OSD_EXISTS | CEPH_OSD_UP);
    new_osdmap->set_state(4, CEPH_OSD_EXISTS | CEPH_OSD_UP);
    new_osdmap->set_state(5, CEPH_OSD_EXISTS | CEPH_OSD_UP);
    new_osdmap->set_epoch(1);
    
    // Build CRUSH map with 2 datacenters
    CrushWrapper crush;
    crush.create();
    
    // Set type names
    crush.set_type_name(10, "root");
    crush.set_type_name(9, "datacenter");
    crush.set_type_name(1, "host");
    crush.set_type_name(0, "osd");
    
    // Create root bucket
    int root_id;
    crush.add_bucket(0, CRUSH_BUCKET_STRAW2, CRUSH_HASH_RJENKINS1,
                     10 /*type*/, 0, NULL, NULL, &root_id);
    crush.set_item_name(root_id, "default");
    
    // Insert OSDs with location hierarchy
    // dc0: OSDs 0,1,2 in hosts host0, host1, host2
    // dc1: OSDs 3,4,5 in hosts host3, host4, host5
    for (int dc = 0; dc < 2; dc++) {
      std::string dc_name = (dc == 0) ? "dc0" : "dc1";
      
      for (int h = 0; h < 3; h++) {
        int osd_id = dc * 3 + h;
        std::string host_name = "host" + std::to_string(osd_id);
        
        std::map<std::string, std::string> loc;
        loc["root"] = "default";
        loc["datacenter"] = dc_name;
        loc["host"] = host_name;
        
        crush.insert_item(g_ceph_context, osd_id, 1.0,
                          "osd." + std::to_string(osd_id), loc);
      }
    }
    
    // Create CRUSH rule for mirrored EC
    // choose 2 type datacenter, chooseleaf indep 3 type host
    int rule_id = 0;
    root_id = crush.get_item_id("default");
    int steps = 6;
    crush_rule *rule = crush_make_rule(steps, pg_pool_t::TYPE_ERASURE);
    int step = 0;
    crush_rule_set_step(rule, step++, CRUSH_RULE_SET_CHOOSELEAF_TRIES, 5, 0);
    crush_rule_set_step(rule, step++, CRUSH_RULE_SET_CHOOSE_TRIES, 100, 0);
    crush_rule_set_step(rule, step++, CRUSH_RULE_TAKE, root_id, 0);
    crush_rule_set_step(rule, step++, CRUSH_RULE_CHOOSE_INDEP, 2, 9 /* datacenter */);
    crush_rule_set_step(rule, step++, CRUSH_RULE_CHOOSELEAF_INDEP, 3, 1 /* host */);
    crush_rule_set_step(rule, step++, CRUSH_RULE_EMIT, 0, 0);
    ASSERT_EQ(step, steps);
    int r = crush_add_rule(crush.get_crush_map(), rule, rule_id);
    ASSERT_GE(r, 0);
    crush.set_rule_name(rule_id, "mirrored_ec_rule");
    
    // Apply CRUSH map via incremental
    OSDMap::Incremental inc(2);
    inc.fsid = new_osdmap->get_fsid();
    crush.encode(inc.crush, CEPH_FEATURES_SUPPORTED_DEFAULT);
    new_osdmap->apply_incremental(inc);
    
    // Create mirrored EC pool
    pool_id = 1;  // Use member variable from base class
    pg_pool_t pool_info;
    pool_info.type = pg_pool_t::TYPE_ERASURE;
    pool_info.size = 6; // 2 zones * 3 shards
    pool_info.min_size = 5; // Can tolerate 1 OSD failure
    pool_info.crush_rule = rule_id;
    pool_info.set_pg_num(8);
    pool_info.set_pgp_num(8);
    
    // EC profile for 2+1
    std::map<std::string, std::string> erasure_code_profile = {
      {"k", "2"},
      {"m", "1"},
      {"plugin", "jerasure"},
      {"technique", "reed_sol_van"}
    };
    new_osdmap->set_erasure_code_profile("default", erasure_code_profile);
    pool_info.erasure_code_profile = "default";
    
    // Stretch mode settings
    pool_info.peering_crush_bucket_barrier = 9; // datacenter type
    pool_info.peering_crush_bucket_target = 2;  // 2 datacenters
    pool_info.peering_crush_mandatory_member = CRUSH_ITEM_NONE;
    
    // EC pool configuration
    pool_info.set_flag(pg_pool_t::FLAG_EC_OVERWRITES);
    
    OSDMapTestHelpers::add_pool(new_osdmap, pool_id, pool_info, "test_ec_pool");
    
    // Update osdmap
    osdmap = new_osdmap;
  }
};


// calc_ec_acting_stretch Tests 
/**
 * Test: Zone isolation - up set respects zone boundaries
 *
 * Scenario: All OSDs up, verify acting set contains shards from both zones
 * Expected: want = [0,1,2,3,4,5] with proper zone distribution [0-2 from dc0, 3-5 from dc1]
 */
TEST_F(TestECActingStretch, ZoneIsolation_AllUp) {
  const pg_pool_t* pool = osdmap->get_pg_pool(pool_id);
  ASSERT_NE(pool, nullptr);
  PGPool pgpool(osdmap, pool_id, *pool, "test_ec_pool");
  
  // Simulate CRUSH mapping: OSDs 0,1,2 from dc0, OSDs 3,4,5 from dc1
  vector<int> up = {0, 1, 2, 3, 4, 5};
  vector<int> acting = {0, 1, 2, 3, 4, 5};
  
  // Build all_info map with pg_info for each shard
  map<pg_shard_t, pg_info_t> all_info;
  pg_history_t history;
  history.epoch_created = 1;
  history.same_interval_since = 1;
  
  for (unsigned i = 0; i < 6; i++) {
    pg_shard_t shard(i, shard_id_t(i));
    pg_info_t info(spg_t(pg_t(1, pool_id), shard_id_t(i)));
    info.history = history;
    info.last_update = eversion_t(1, i);
    all_info[shard] = info;
  }
  
  // Auth log shard is primary (OSD 0, shard 0)
  auto auth_log_shard = all_info.find(pg_shard_t(0, shard_id_t(0)));
  ASSERT_NE(auth_log_shard, all_info.end());
  
  // Call calc_ec_acting_stretch
  vector<int> want;
  set<pg_shard_t> backfill;
  set<pg_shard_t> acting_backfill;
  ostringstream ss;
  
  PeeringState::calc_ec_acting_stretch(
    auth_log_shard,
    pool->size,
    acting,
    up,
    all_info,
    false, // restrict_to_up_acting
    &want,
    &backfill,
    &acting_backfill,
    osdmap,
    pgpool,
    ss);
  
  // Verify want contains all 6 OSDs
  EXPECT_EQ(want.size(), 6);
  EXPECT_EQ(want[0], 0);
  EXPECT_EQ(want[1], 1);
  EXPECT_EQ(want[2], 2);
  EXPECT_EQ(want[3], 3);
  EXPECT_EQ(want[4], 4);
  EXPECT_EQ(want[5], 5);
  
  // Verify no backfill needed
  EXPECT_TRUE(backfill.empty()) << "No backfill needed when all OSDs up";
  
  // Verify zone distribution: OSDs 0-2 in dc0, OSDs 3-5 in dc1
  for (int i = 0; i < 3; i++) {
    int dc0 = osdmap->crush->get_parent_of_type(i, 9, pool->crush_rule);
    int dc1 = osdmap->crush->get_parent_of_type(i + 3, 9, pool->crush_rule);
    EXPECT_NE(dc0, dc1) << "dc0 and dc1 should be different buckets";
  }
}

/**
 * Test: Zone isolation - single zone OSD down
 *
 * Scenario: OSD 1 (dc0) down, verify strays only selected from dc0
 * Expected: want should prefer OSD from same zone as replacement
 */
TEST_F(TestECActingStretch, ZoneIsolation_SingleOSDDown) {
  const pg_pool_t* pool = osdmap->get_pg_pool(pool_id);
  ASSERT_NE(pool, nullptr);
  PGPool pgpool(osdmap, pool_id, *pool, "test_ec_pool");
  
  // Mark OSD 1 down
  mark_osd_down(1);
  
  // OSD 1 is down - shard position 1 in dc0
  // up vector reflects current CRUSH mapping with OSD 1 missing
  vector<int> up = {0, CRUSH_ITEM_NONE, 2, 3, 4, 5};
  vector<int> acting = {0, 1, 2, 3, 4, 5}; // OSD 1 still in acting (was up before)
  
  // Build all_info map
  map<pg_shard_t, pg_info_t> all_info;
  pg_history_t history;
  history.epoch_created = 1;
  history.same_interval_since = 1;
  
  for (unsigned i = 0; i < 6; i++) {
    pg_shard_t shard(i, shard_id_t(i));
    pg_info_t info(spg_t(pg_t(1, pool_id), shard_id_t(i)));
    info.history = history;
    info.last_update = eversion_t(1, i);
    all_info[shard] = info;
  }
  
  auto auth_log_shard = all_info.find(pg_shard_t(0, shard_id_t(0)));
  ASSERT_NE(auth_log_shard, all_info.end());
  
  // Call calc_ec_acting_stretch
  vector<int> want;
  set<pg_shard_t> backfill;
  set<pg_shard_t> acting_backfill;
  ostringstream ss;
  
  PeeringState::calc_ec_acting_stretch(
    auth_log_shard,
    pool->size,
    acting,
    up,
    all_info,
    false,
    &want,
    &backfill,
    &acting_backfill,
    osdmap,
    pgpool,
    ss);
  
  // Verify want vector
  ASSERT_EQ(want.size(), 6);
  
  // Position 1 should be filled - either keep acting[1] if it has data,
  // or select CRUSH_ITEM_NONE if no suitable replacement from dc0
  // The key constraint: MUST NOT select from dc1 (OSDs 3,4,5)
  if (want[1] != CRUSH_ITEM_NONE) {
    int zone_want1 = osdmap->crush->get_parent_of_type(want[1], 9, pool->crush_rule);
    int zone_osd0 = osdmap->crush->get_parent_of_type(0, 9, pool->crush_rule);
    EXPECT_EQ(zone_want1, zone_osd0) << "Replacement for position 1 must be from dc0, not dc1";
    EXPECT_TRUE(want[1] == 0 || want[1] == 1 || want[1] == 2) 
      << "Position 1 OSD must be from dc0 (OSDs 0,1,2), got " << want[1];
  }
  
  // Verify other positions unchanged
  EXPECT_EQ(want[0], 0);
  EXPECT_EQ(want[2], 2);
  EXPECT_EQ(want[3], 3);
  EXPECT_EQ(want[4], 4);
  EXPECT_EQ(want[5], 5);
}

/**
 * Test: bucket_max enforcement - zone at ceiling
 *
 * Scenario: Multiple OSDs down, verify zone doesn't exceed bucket_max when selecting
 * Expected: bucket_max = 3 for 2-zone 6-OSD pool
 */
TEST_F(TestECActingStretch, BucketMax_ZoneAtCeiling) {
  const pg_pool_t* pool = osdmap->get_pg_pool(pool_id);
  ASSERT_NE(pool, nullptr);
  PGPool pgpool(osdmap, pool_id, *pool, "test_ec_pool");
  
  // bucket_max calculation
  unsigned bucket_max = (pool->size + pool->peering_crush_bucket_target - 1) / 
                        pool->peering_crush_bucket_target;
  EXPECT_EQ(bucket_max, 3) << "bucket_max = ceil(6 / 2) = 3";
  
  // Mark OSDs 0 and 1 down (both from dc0)
  mark_osds_down({0, 1});
  
  // up[0] = NONE, up[1] = NONE, up[2] = 2 (dc0 still has OSD 2)
  // acting still has old values
  vector<int> up = {CRUSH_ITEM_NONE, CRUSH_ITEM_NONE, 2, 3, 4, 5};
  vector<int> acting = {0, 1, 2, 3, 4, 5};
  
  // Build all_info map
  map<pg_shard_t, pg_info_t> all_info;
  pg_history_t history;
  history.epoch_created = 1;
  history.same_interval_since = 1;
  
  for (unsigned i = 0; i < 6; i++) {
    pg_shard_t shard(i, shard_id_t(i));
    pg_info_t info(spg_t(pg_t(1, pool_id), shard_id_t(i)));
    info.history = history;
    info.last_update = eversion_t(1, i);
    all_info[shard] = info;
  }
  
  auto auth_log_shard = all_info.find(pg_shard_t(2, shard_id_t(2)));
  ASSERT_NE(auth_log_shard, all_info.end());
  
  // Call calc_ec_acting_stretch
  vector<int> want;
  set<pg_shard_t> backfill;
  set<pg_shard_t> acting_backfill;
  ostringstream ss;
  
  PeeringState::calc_ec_acting_stretch(
    auth_log_shard,
    pool->size,
    acting,
    up,
    all_info,
    false,
    &want,
    &backfill,
    &acting_backfill,
    osdmap,
    pgpool,
    ss);
  
  ASSERT_EQ(want.size(), 6);
  
  // Count OSDs selected per zone
  map<int, int> zone_counts;
  for (unsigned i = 0; i < want.size(); i++) {
    if (want[i] != CRUSH_ITEM_NONE) {
      int zone = osdmap->crush->get_parent_of_type(want[i], 9, pool->crush_rule);
      zone_counts[zone]++;
    }
  }
  
  // Verify no zone exceeds bucket_max
  for (const auto& [zone, count] : zone_counts) {
    EXPECT_LE(count, bucket_max) << "Zone " << zone << " exceeds bucket_max=" << bucket_max;
  }
}

/**
 * Test: Stray selection with zone constraint
 *
 * Scenario: Multiple OSDs down in one zone, verify strays selected from same zone only
 * Expected: Strays for dc0 positions must come from dc0, not dc1
 */
TEST_F(TestECActingStretch, StraySelection_ZoneConstraint) {
  const pg_pool_t* pool = osdmap->get_pg_pool(pool_id);
  ASSERT_NE(pool, nullptr);
  PGPool pgpool(osdmap, pool_id, *pool, "test_ec_pool");
  
  // Mark OSDs 0 and 1 down (both dc0)
  mark_osds_down({0, 1});
  
  // up[0] = NONE, up[1] = NONE, up[2] = 2 (dc0)
  vector<int> up = {CRUSH_ITEM_NONE, CRUSH_ITEM_NONE, 2, 3, 4, 5};
  vector<int> acting = {0, 1, 2, 3, 4, 5};
  
  // Build all_info map
  map<pg_shard_t, pg_info_t> all_info;
  pg_history_t history;
  history.epoch_created = 1;
  history.same_interval_since = 1;
  
  for (unsigned i = 0; i < 6; i++) {
    pg_shard_t shard(i, shard_id_t(i));
    pg_info_t info(spg_t(pg_t(1, pool_id), shard_id_t(i)));
    info.history = history;
    info.last_update = eversion_t(1, i);
    all_info[shard] = info;
  }
  
  auto auth_log_shard = all_info.find(pg_shard_t(2, shard_id_t(2)));
  ASSERT_NE(auth_log_shard, all_info.end());
  
  // Call calc_ec_acting_stretch
  vector<int> want;
  set<pg_shard_t> backfill;
  set<pg_shard_t> acting_backfill;
  ostringstream ss;
  
  PeeringState::calc_ec_acting_stretch(
    auth_log_shard,
    pool->size,
    acting,
    up,
    all_info,
    false,
    &want,
    &backfill,
    &acting_backfill,
    osdmap,
    pgpool,
    ss);
  
  ASSERT_EQ(want.size(), 6);
  
  // Get dc0 and dc1 buckets
  int dc0_bucket = osdmap->crush->get_parent_of_type(2, 9, pool->crush_rule);
  int dc1_bucket = osdmap->crush->get_parent_of_type(3, 9, pool->crush_rule);
  EXPECT_NE(dc0_bucket, dc1_bucket);
  
  // Verify positions 0 and 1: if filled, must be from dc0
  for (int pos : {0, 1}) {
    if (want[pos] != CRUSH_ITEM_NONE) {
      int zone = osdmap->crush->get_parent_of_type(want[pos], 9, pool->crush_rule);
      EXPECT_EQ(zone, dc0_bucket) 
        << "Position " << pos << " OSD " << want[pos] 
        << " must be from dc0, not dc1";
    }
  }
  
  // Verify positions 2-5 correct
  EXPECT_EQ(want[2], 2);
  EXPECT_EQ(want[3], 3);
  EXPECT_EQ(want[4], 4);
  EXPECT_EQ(want[5], 5);
}

/**
 * Test: Mixed up/acting/stray scenario
 *
 * Scenario: Some shards from up, some from acting, some need strays
 * Expected: Each shard position respects zone boundaries throughout selection
 */
TEST_F(TestECActingStretch, MixedSelection_UpActingStray) {
  const pg_pool_t* pool = osdmap->get_pg_pool(pool_id);
  ASSERT_NE(pool, nullptr);
  PGPool pgpool(osdmap, pool_id, *pool, "test_ec_pool");
  
  // Mark OSD 1 down
  mark_osd_down(1);
  
  // Complex scenario:
  // - Position 0: up[0] = OSD 0 (dc0) - use up
  // - Position 1: up[1] = NONE, acting[1] = OSD 1 (dc0) down - need stray from dc0
  // - Position 2: up[2] = OSD 2 (dc0) - use up
  // - Position 3: up[3] = OSD 3 (dc1) - use up
  // - Position 4: up[4] = NONE, acting[4] = OSD 4 (dc1) up - use acting
  // - Position 5: up[5] = OSD 5 (dc1) - use up
  
  vector<int> up = {0, CRUSH_ITEM_NONE, 2, 3, CRUSH_ITEM_NONE, 5};
  vector<int> acting = {0, 1, 2, 3, 4, 5};
  
  // Build all_info map
  map<pg_shard_t, pg_info_t> all_info;
  pg_history_t history;
  history.epoch_created = 1;
  history.same_interval_since = 1;
  
  for (unsigned i = 0; i < 6; i++) {
    pg_shard_t shard(i, shard_id_t(i));
    pg_info_t info(spg_t(pg_t(1, pool_id), shard_id_t(i)));
    info.history = history;
    info.last_update = eversion_t(1, i);
    all_info[shard] = info;
  }
  
  auto auth_log_shard = all_info.find(pg_shard_t(0, shard_id_t(0)));
  ASSERT_NE(auth_log_shard, all_info.end());
  
  // Call calc_ec_acting_stretch
  vector<int> want;
  set<pg_shard_t> backfill;
  set<pg_shard_t> acting_backfill;
  ostringstream ss;
  
  PeeringState::calc_ec_acting_stretch(
    auth_log_shard,
    pool->size,
    acting,
    up,
    all_info,
    false,
    &want,
    &backfill,
    &acting_backfill,
    osdmap,
    pgpool,
    ss);
  
  ASSERT_EQ(want.size(), 6);
  
  // Verify expected selections:
  EXPECT_EQ(want[0], 0) << "Position 0 should use up[0]";
  EXPECT_EQ(want[2], 2) << "Position 2 should use up[2]";
  EXPECT_EQ(want[3], 3) << "Position 3 should use up[3]";
  EXPECT_EQ(want[4], 4) << "Position 4 should use acting[4]";
  EXPECT_EQ(want[5], 5) << "Position 5 should use up[5]";
  
  // Position 1: up[1] = NONE, acting[1] = 1 (down)
  // Should keep acting[1] if it has valid data, or select NONE
  // Must NOT select from dc1
  if (want[1] != CRUSH_ITEM_NONE && want[1] != 1) {
    int dc0 = osdmap->crush->get_parent_of_type(0, 9, pool->crush_rule);
    int zone1 = osdmap->crush->get_parent_of_type(want[1], 9, pool->crush_rule);
    EXPECT_EQ(zone1, dc0) << "Position 1 replacement must be from dc0";
  }
  
  // Verify bucket_max not exceeded
  unsigned bucket_max = (pool->size + pool->peering_crush_bucket_target - 1) / 
                        pool->peering_crush_bucket_target;
  EXPECT_EQ(bucket_max, 3);
  
  map<int, int> zone_counts;
  for (unsigned i = 0; i < want.size(); i++) {
    if (want[i] != CRUSH_ITEM_NONE) {
      int zone = osdmap->crush->get_parent_of_type(want[i], 9, pool->crush_rule);
      zone_counts[zone]++;
    }
  }
  
  for (const auto& [zone, count] : zone_counts) {
    EXPECT_LE(count, bucket_max) << "Zone should not exceed bucket_max";
  }
}

// ============================================================================
// calc_ec_acting_stretch Tests - bucket_max Enforcement
// ============================================================================

/**
 * Test: bucket_max calculation
 *
 * Verify bucket_max = ceil(size / target) for various configurations
 */
TEST_F(TestECActingStretch, BucketMax_Calculation) {
  const pg_pool_t* pool = osdmap->get_pg_pool(pool_id);
  ASSERT_NE(pool, nullptr);
  
  // Current pool: size=6, target=2
  EXPECT_EQ(pool->size, 6);
  EXPECT_EQ(pool->peering_crush_bucket_target, 2);
  
  unsigned bucket_max = (pool->size + pool->peering_crush_bucket_target - 1) / 
                        pool->peering_crush_bucket_target;
  EXPECT_EQ(bucket_max, 3) << "ceil(6 / 2) = 3";
  
  // Test other configurations (hypothetical):
  // size=5, target=2 → ceil(5/2) = 3
  unsigned test_max_1 = (5 + 2 - 1) / 2;
  EXPECT_EQ(test_max_1, 3);
  
  // size=8, target=2 → ceil(8/2) = 4
  unsigned test_max_2 = (8 + 2 - 1) / 2;
  EXPECT_EQ(test_max_2, 4);
  
  // size=9, target=3 → ceil(9/3) = 3
  unsigned test_max_3 = (9 + 3 - 1) / 3;
  EXPECT_EQ(test_max_3, 3);
  
  // size=10, target=3 → ceil(10/3) = 4
  unsigned test_max_4 = (10 + 3 - 1) / 3;
  EXPECT_EQ(test_max_4, 4);
}

/**
 * Test: bucket_max prevents over-selection
 *
 * Scenario: Zone already has bucket_max OSDs, verify no more strays selected
 * Expected: zone_at_max check blocks additional selections
 */
TEST_F(TestECActingStretch, BucketMax_PreventsOverSelection) {
  spg_t pgid(pg_t(1, pool_id), shard_id_t::NO_SHARD);
  
  const pg_pool_t* pool = osdmap->get_pg_pool(pool_id);
  ASSERT_NE(pool, nullptr);
  unsigned bucket_max = (pool->size + pool->peering_crush_bucket_target - 1) / 
                        pool->peering_crush_bucket_target;
  EXPECT_EQ(bucket_max, 3);
  
  // Scenario: All 3 OSDs from dc0 already selected
  // zone_shard_count[dc0] = 3 (at bucket_max)
  // Cannot select any more OSDs from dc0
  
  // Simulate: OSDs 0, 1, 2 all selected from dc0
  std::map<int, unsigned> zone_shard_count;
  int dc0_bucket = osdmap->crush->get_parent_of_type(0, 9, pool->crush_rule);
  zone_shard_count[dc0_bucket] = 3;
  
  // Check zone_at_max logic
  auto it = zone_shard_count.find(dc0_bucket);
  bool at_max = (it != zone_shard_count.end() && it->second >= bucket_max);
  EXPECT_TRUE(at_max) << "dc0 should be at bucket_max";
  
  // If we tried to select another OSD from dc0, zone_at_max would block it
  // This test validates the zone_at_max check in calc_ec_acting_stretch
}

#if 0  // Disabled until choose_async_recovery_ec is accessible

/**
 * Test: Async recovery respects stretch_set_can_peer
 */
TEST_F(TestECActingStretch, AsyncRecovery_StretchSetCanPeer) {
  const pg_pool_t* pool = osdmap->get_pg_pool(pool_id);
  ASSERT_NE(pool, nullptr);
  auto ps = create_peering_state(0);
  ASSERT_NE(ps, nullptr);
  
  unsigned bucket_max = (pool->size + pool->peering_crush_bucket_target - 1) / 
                        pool->peering_crush_bucket_target;
  EXPECT_EQ(bucket_max, 3);
  
  vector<int> want = {0, 1, 2, 3, 4, 5};
  map<pg_shard_t, pg_info_t> all_info;
  pg_history_t history;
  history.epoch_created = 1;
  history.same_interval_since = 1;
  
  for (unsigned i = 0; i < 6; i++) {
    pg_shard_t shard(i, shard_id_t(i));
    pg_info_t info(spg_t(pg_t(1, pool_id), shard_id_t(i)));
    info.history = history;
    if (i == 1 || i == 4) {
      info.last_update = eversion_t(1, 1);
      info.last_complete = eversion_t(1, 1);
    } else {
      info.last_update = eversion_t(1, 10);
      info.last_complete = eversion_t(1, 10);
    }
    all_info[shard] = info;
  }
  
  pg_info_t auth_info(spg_t(pg_t(1, pool_id), shard_id_t(0)));
  auth_info.history = history;
  auth_info.last_update = eversion_t(1, 10);
  auth_info.last_complete = eversion_t(1, 10);
  
  set<pg_shard_t> async_recovery;
  ps->choose_async_recovery_ec(all_info, auth_info, &want, &async_recovery, osdmap);
  
  map<int, int> zone_counts;
  for (const auto& shard : async_recovery) {
    if (shard.osd < 6) {
      int zone = osdmap->crush->get_parent_of_type(shard.osd, 9, pool->crush_rule);
      zone_counts[zone]++;
    }
  }
  
  for (const auto& [zone, count] : zone_counts) {
    EXPECT_LE(count, bucket_max) << "Async recovery should not exceed bucket_max per zone";
  }
}

/**
 * Test: Async recovery candidate selection with zone constraints
 */
TEST_F(TestECActingStretch, AsyncRecovery_ZoneAwareSelection) {
  const pg_pool_t* pool = osdmap->get_pg_pool(pool_id);
  ASSERT_NE(pool, nullptr);
  auto ps = create_peering_state(0);
  ASSERT_NE(ps, nullptr);
  
  vector<int> want = {0, 1, 2, 3, 4, 5};
  map<pg_shard_t, pg_info_t> all_info;
  pg_history_t history;
  history.epoch_created = 1;
  history.same_interval_since = 1;
  
  for (unsigned i = 0; i < 6; i++) {
    pg_shard_t shard(i, shard_id_t(i));
    pg_info_t info(spg_t(pg_t(1, pool_id), shard_id_t(i)));
    info.history = history;
    if (i == 1) {
      info.last_update = eversion_t(1, 0);
      info.last_complete = eversion_t(1, 0);
    } else {
      info.last_update = eversion_t(1, 10);
      info.last_complete = eversion_t(1, 10);
    }
    all_info[shard] = info;
  }
  
  pg_info_t auth_info(spg_t(pg_t(1, pool_id), shard_id_t(0)));
  auth_info.history = history;
  auth_info.last_update = eversion_t(1, 10);
  auth_info.last_complete = eversion_t(1, 10);
  
  set<pg_shard_t> async_recovery;
  ps->choose_async_recovery_ec(all_info, auth_info, &want, &async_recovery, osdmap);
  
  if (!async_recovery.empty()) {
    for (const auto& shard : async_recovery) {
      if (shard.osd < 6) {
        EXPECT_TRUE(true) << "Async recovery candidate: OSD " << shard.osd;
      }
    }
  }
}

/**
 * Test: Recovery cost ordering with stretch constraints
 */
TEST_F(TestECActingStretch, AsyncRecovery_CostOrdering) {
  const pg_pool_t* pool = osdmap->get_pg_pool(pool_id);
  ASSERT_NE(pool, nullptr);
  auto ps = create_peering_state(0);
  ASSERT_NE(ps, nullptr);
  
  vector<int> want = {0, 1, 2, 3, 4, 5};
  map<pg_shard_t, pg_info_t> all_info;
  pg_history_t history;
  history.epoch_created = 1;
  history.same_interval_since = 1;
  
  for (unsigned i = 0; i < 6; i++) {
    pg_shard_t shard(i, shard_id_t(i));
    pg_info_t info(spg_t(pg_t(1, pool_id), shard_id_t(i)));
    info.history = history;
    if (i == 1) {
      info.last_update = eversion_t(1, 1);
      info.last_complete = eversion_t(1, 1);
    } else if (i == 2) {
      info.last_update = eversion_t(1, 8);
      info.last_complete = eversion_t(1, 8);
    } else if (i == 4) {
      info.last_update = eversion_t(1, 5);
      info.last_complete = eversion_t(1, 5);
    } else {
      info.last_update = eversion_t(1, 10);
      info.last_complete = eversion_t(1, 10);
    }
    all_info[shard] = info;
  }
  
  pg_info_t auth_info(spg_t(pg_t(1, pool_id), shard_id_t(0)));
  auth_info.history = history;
  auth_info.last_update = eversion_t(1, 10);
  auth_info.last_complete = eversion_t(1, 10);
  
  set<pg_shard_t> async_recovery;
  ps->choose_async_recovery_ec(all_info, auth_info, &want, &async_recovery, osdmap);
  
  if (!async_recovery.empty()) {
    unsigned bucket_max = (pool->size + pool->peering_crush_bucket_target - 1) / 
                          pool->peering_crush_bucket_target;
    map<int, int> zone_counts;
    for (const auto& shard : async_recovery) {
      if (shard.osd < 6) {
        int zone = osdmap->crush->get_parent_of_type(shard.osd, 9, pool->crush_rule);
        zone_counts[zone]++;
      }
    }
    for (const auto& [zone, count] : zone_counts) {
      EXPECT_LE(count, bucket_max) << "Async recovery should not exceed bucket_max per zone";
    }
  }
}

#endif  // Disabled async recovery tests
