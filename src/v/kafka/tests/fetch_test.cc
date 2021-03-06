// Copyright 2020 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "kafka/requests/fetch_request.h"
#include "model/fundamental.h"
#include "redpanda/tests/fixture.h"
#include "resource_mgmt/io_priority.h"
#include "test_utils/async.h"

#include <seastar/core/smp.hh>

#include <chrono>
#include <limits>

using namespace std::chrono_literals;

SEASTAR_THREAD_TEST_CASE(partition_iterator) {
    /*
     * extract topic partitions from the request
     */
    auto transform = [](const kafka::fetch_request& request) {
        std::vector<model::topic_partition> res;
        std::transform(
          request.cbegin(),
          request.cend(),
          std::back_inserter(res),
          [&res](const kafka::fetch_request::const_iterator::value_type& v) {
              if (v.new_topic) {
                  if (!res.empty()) {
                      BOOST_TEST(res.back().topic != v.topic->name);
                  }
              } else {
                  BOOST_TEST(!res.empty());
                  BOOST_TEST(res.back().topic == v.topic->name);
              }
              return model::topic_partition(v.topic->name, v.partition->id);
          });
        return res;
    };

    {
        // no topics -> empty
        kafka::fetch_request req;
        auto parts = transform(req);
        BOOST_TEST(parts.empty());
    }

    {
        // 1 topic, no partitions -> empty
        kafka::fetch_request req;
        req.topics.push_back({.name = model::topic("t0")});
        auto parts = transform(req);
        BOOST_TEST(parts.empty());
    }

    {
        // 2 topics, no partitions -> empty
        kafka::fetch_request req;
        req.topics.push_back({.name = model::topic("t0")});
        req.topics.push_back({.name = model::topic("t1")});
        auto parts = transform(req);
        BOOST_TEST(parts.empty());
    }

    {
        // 1 topic, 1 partition
        kafka::fetch_request req;
        req.topics.push_back({
          .name = model::topic("t0"),
          .partitions = {{.id = model::partition_id(100)}},
        });
        auto parts = transform(req);
        BOOST_TEST(parts.size() == 1);
        BOOST_TEST(parts[0].topic == model::topic("t0"));
        BOOST_TEST(parts[0].partition == 100);
    }

    {
        // 1 topic, 2 partitions
        kafka::fetch_request req;
        req.topics.push_back(
          {.name = model::topic("t0"),
           .partitions = {
             {.id = model::partition_id(100)},
             {.id = model::partition_id(101)}}});
        auto parts = transform(req);
        BOOST_TEST(parts.size() == 2);
        BOOST_TEST(parts[0].topic == model::topic("t0"));
        BOOST_TEST(parts[0].partition == 100);
        BOOST_TEST(parts[1].topic == model::topic("t0"));
        BOOST_TEST(parts[1].partition == 101);
    }

    {
        // 2 topics, 2/1 partition
        kafka::fetch_request req;
        req.topics.push_back(
          {.name = model::topic("t0"),
           .partitions = {
             {.id = model::partition_id(100)},
             {.id = model::partition_id(101)}}});
        req.topics.push_back(
          {.name = model::topic("t1"),
           .partitions = {{.id = model::partition_id(102)}}});
        auto parts = transform(req);
        BOOST_TEST(parts.size() == 3);
        BOOST_TEST(parts[0].topic == model::topic("t0"));
        BOOST_TEST(parts[0].partition == 100);
        BOOST_TEST(parts[1].topic == model::topic("t0"));
        BOOST_TEST(parts[1].partition == 101);
        BOOST_TEST(parts[2].topic == model::topic("t1"));
        BOOST_TEST(parts[2].partition == 102);
    }

    {
        // 4 topics, 2/{}/{}/2 partition
        kafka::fetch_request req;
        req.topics.push_back(
          {.name = model::topic("t0"),
           .partitions = {
             {.id = model::partition_id(100)},
             {.id = model::partition_id(101)}}});
        req.topics.push_back({.name = model::topic("t1")});
        req.topics.push_back({.name = model::topic("t2")});
        req.topics.push_back(
          {.name = model::topic("t3"),
           .partitions = {
             {.id = model::partition_id(102)},
             {.id = model::partition_id(103)}}});
        auto parts = transform(req);
        BOOST_TEST(parts.size() == 4);
        BOOST_TEST(parts[0].topic == model::topic("t0"));
        BOOST_TEST(parts[0].partition == 100);
        BOOST_TEST(parts[1].topic == model::topic("t0"));
        BOOST_TEST(parts[1].partition == 101);
        BOOST_TEST(parts[2].topic == model::topic("t3"));
        BOOST_TEST(parts[2].partition == 102);
        BOOST_TEST(parts[3].topic == model::topic("t3"));
        BOOST_TEST(parts[3].partition == 103);
    }
}

// TODO: this sort of factory should eventually go into a kafka fixture that
// builds on top of the redpanda application fixture.
static kafka::request_context make_request_context(application& app) {
    kafka::request_header header;
    auto encoder_context = kafka::request_context(
      app.metadata_cache,
      app.controller->get_topics_frontend().local(),
      std::move(header),
      iobuf(),
      std::chrono::milliseconds(0),
      app.group_router.local(),
      app.shard_table.local(),
      app.partition_manager,
      app.coordinator_ntp_mapper);

    iobuf buf;
    kafka::fetch_request request;
    request.max_wait_time = std::chrono::milliseconds::zero();
    kafka::response_writer writer(buf);
    request.encode(writer, encoder_context.header().version);

    return kafka::request_context(
      app.metadata_cache,
      app.controller->get_topics_frontend().local(),
      std::move(header),
      std::move(buf),
      std::chrono::milliseconds(0),
      app.group_router.local(),
      app.shard_table.local(),
      app.partition_manager,
      app.coordinator_ntp_mapper);
}

// TODO: when we have a more precise log builder tool we can make these finer
// grained tests. for now the test is coarse grained based on the random batch
// builder.
FIXTURE_TEST(read_from_ntp_max_bytes, redpanda_thread_fixture) {
    auto do_read = [this](model::ntp ntp, size_t max_bytes) {
        kafka::fetch_config config{
          .start_offset = model::offset(0),
          .max_bytes = max_bytes,
          .timeout = model::no_timeout,
        };
        auto rctx = make_request_context(app);
        auto octx = kafka::op_context(
          std::move(rctx), ss::default_smp_service_group());
        auto resp = kafka::read_from_ntp(octx, ntp, config).get0();
        BOOST_REQUIRE(resp.record_set);
        return resp;
    };
    wait_for_controller_leadership().get0();
    auto ntp = make_data(storage::ntp_config::ntp_id(2));

    auto shard = app.shard_table.local().shard_for(ntp);
    tests::cooperative_spin_wait_with_timeout(10s, [this, shard, ntp = ntp] {
        return app.partition_manager.invoke_on(
          *shard, [ntp](cluster::partition_manager& mgr) {
              auto partition = mgr.get(ntp);
              return partition
                     && partition->committed_offset() >= model::offset(1);
          });
    }).get();

    auto zero = do_read(ntp, 0).record_set->size_bytes();
    auto one = do_read(ntp, 1).record_set->size_bytes();
    auto maxlimit = do_read(ntp, std::numeric_limits<size_t>::max())
                      .record_set->size_bytes();

    BOOST_TEST(zero > 0); // read something
    BOOST_TEST(zero == one);
    BOOST_TEST(one <= maxlimit); // read more
}

FIXTURE_TEST(fetch_one, redpanda_thread_fixture) {
    // create a topic partition with some data
    model::topic topic("foo");
    model::partition_id pid(0);
    model::offset offset(0);
    auto ntp = make_default_ntp(topic, pid);
    auto log_config = make_default_config();
    {
        using namespace storage;
        storage::disk_log_builder builder(log_config);
        storage::ntp_config ntp_cfg(
          ntp, log_config.base_dir, nullptr, storage::ntp_config::ntp_id(2));
        builder | start(std::move(ntp_cfg)) | add_segment(model::offset(0))
          | add_random_batch(model::offset(0), 10, maybe_compress_batches::yes)
          | stop();
    }
    wait_for_controller_leadership().get0();

    add_topic(model::topic_namespace_view(ntp)).get();
    auto shard = app.shard_table.local().shard_for(ntp);

    tests::cooperative_spin_wait_with_timeout(10s, [this, shard, ntp] {
        return app.partition_manager.invoke_on(
          *shard, [ntp](cluster::partition_manager& mgr) {
              auto partition = mgr.get(ntp);
              return partition
                     && partition->committed_offset() >= model::offset(1);
          });
    }).get();

    kafka::fetch_request req;
    req.max_bytes = std::numeric_limits<int32_t>::max();
    req.min_bytes = 1;
    req.max_wait_time = std::chrono::milliseconds(0);
    req.topics = {{
      .name = topic,
      .partitions = {{
        .id = pid,
        .fetch_offset = offset,
      }},
    }};

    auto client = make_kafka_client().get0();
    client.connect().get();
    auto resp = client.dispatch(req, kafka::api_version(4)).get0();
    client.stop().then([&client] { client.shutdown(); }).get();

    BOOST_REQUIRE(resp.partitions.size() == 1);
    BOOST_REQUIRE(resp.partitions[0].name == topic());
    BOOST_REQUIRE(resp.partitions[0].responses.size() == 1);
    BOOST_REQUIRE(
      resp.partitions[0].responses[0].error == kafka::error_code::none);
    BOOST_REQUIRE(resp.partitions[0].responses[0].id == pid);
    BOOST_REQUIRE(resp.partitions[0].responses[0].record_set);
    BOOST_REQUIRE(resp.partitions[0].responses[0].record_set->size_bytes() > 0);
}
