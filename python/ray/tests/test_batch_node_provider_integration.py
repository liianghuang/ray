"""Integration/e2e test for BatchingNodeProvider.
Adapts FakeMultiNodeProvider tests.
"""
from copy import deepcopy
import sys

import pytest


import ray
from ray._common.test_utils import wait_for_condition
from ray.autoscaler.batching_node_provider import (
    BatchingNodeProvider,
    NodeData,
    ScaleRequest,
)
from ray.autoscaler._private.fake_multi_node.node_provider import FakeMultiNodeProvider
from ray.autoscaler._private.constants import FOREGROUND_NODE_LAUNCH_KEY
from ray.autoscaler.tags import (
    NODE_KIND_WORKER,
    STATUS_UP_TO_DATE,
    TAG_RAY_NODE_KIND,
    TAG_RAY_NODE_STATUS,
    TAG_RAY_USER_NODE_TYPE,
)
from ray.cluster_utils import AutoscalingCluster


import logging

logger = logging.getLogger(__name__)


class FakeBatchingNodeProvider(BatchingNodeProvider):
    """Class for e2e local testing of BatchingNodeProvider.
    Uses FakeMultiNodeProvider as a proxy for managing the nodes.

    This node provider requires the "available_node_types" section of the
    autoscaling config to be copied into the "provider" section.
    That's needed so that node resources can be accessed as
    provider_config["available_node_types"][node_type]["resources"].

    See the create_node_with_resources call in submit_scale_request.
    See class BatchingAutoscaler below.
    """

    def __init__(self, provider_config, cluster_name):
        BatchingNodeProvider.__init__(self, provider_config, cluster_name)
        self.fake_multi_node_provider = FakeMultiNodeProvider(
            provider_config, cluster_name
        )

    # Manually "inherit" internal utility functions.
    # I prefer this over attempting multiple inheritance.
    def _next_hex_node_id(self):
        return self.fake_multi_node_provider._next_hex_node_id()

    def _terminate_node(self, node):
        return self.fake_multi_node_provider._terminate_node(node)

    def get_node_data(self):
        node_data_dict = {}
        for node_id in self.fake_multi_node_provider._nodes:
            tags = self.fake_multi_node_provider._nodes[node_id]["tags"]
            node_data_dict[node_id] = NodeData(
                kind=tags[TAG_RAY_NODE_KIND],
                type=tags[TAG_RAY_USER_NODE_TYPE],
                status=tags[TAG_RAY_NODE_STATUS],
                ip=node_id,
            )
        return node_data_dict

    def submit_scale_request(self, scale_request: ScaleRequest):
        worker_counts = self.cur_num_workers()
        for worker_to_delete in scale_request.workers_to_delete:
            node_type = self.node_tags(worker_to_delete)[TAG_RAY_USER_NODE_TYPE]
            FakeMultiNodeProvider.terminate_node(self, worker_to_delete)
            worker_counts[node_type] -= 1
        for node_type in scale_request.desired_num_workers:
            diff = (
                scale_request.desired_num_workers[node_type] - worker_counts[node_type]
            )
            # It is non-standard for "available_node_types" to be included in the
            # provider config, but it is necessary for this node provider.
            resources = self.provider_config["available_node_types"][node_type][
                "resources"
            ]
            labels = self.provider_config["available_node_types"][node_type].get(
                "labels", {}
            )
            tags = {
                TAG_RAY_NODE_KIND: NODE_KIND_WORKER,
                TAG_RAY_USER_NODE_TYPE: node_type,
                TAG_RAY_NODE_STATUS: STATUS_UP_TO_DATE,
            }
            self.fake_multi_node_provider.create_node_with_resources_and_labels(
                node_config={},
                tags=tags,
                count=diff,
                resources=resources,
                labels=labels,
            )


class BatchingAutoscalingCluster(AutoscalingCluster):
    """Class used for e2e testing of BatchingNodeProvider.
    Like AutoscalingCluster but uses a BatchingNodePorvider.
    """

    def _generate_config(self, head_resources, worker_node_types, **config_kwargs):
        config = AutoscalingCluster._generate_config(
            self, head_resources, worker_node_types
        )
        # Need this for resource data.
        config["provider"]["available_node_types"] = deepcopy(
            config["available_node_types"]
        )
        # Load the node provider class above.
        config["provider"]["type"] = "external"
        config["provider"][
            "module"
        ] = "ray.tests.test_batch_node_provider_integration.FakeBatchingNodeProvider"
        # Need to run in single threaded mode to use BatchingNodeProvider.
        config["provider"][FOREGROUND_NODE_LAUNCH_KEY] = True
        return config


@pytest.mark.skipif(sys.platform.startswith("win"), reason="Not relevant on Windows.")
def test_fake_batching_autoscaler_e2e(shutdown_only):
    cluster = BatchingAutoscalingCluster(
        head_resources={"CPU": 2},
        worker_node_types={
            "cpu_node": {
                "resources": {
                    "CPU": 4,
                    "object_store_memory": 1024 * 1024 * 1024,
                },
                "node_config": {},
                "min_workers": 0,
                "max_workers": 2,
            },
            "gpu_node": {
                "resources": {
                    "CPU": 2,
                    "GPU": 1,
                    "object_store_memory": 1024 * 1024 * 1024,
                },
                "node_config": {},
                "min_workers": 0,
                "max_workers": 2,
            },
        },
    )

    try:
        cluster.start()
        ray.init("auto")

        # Triggers the addition of a GPU node.
        @ray.remote(num_gpus=1)
        def f():
            print("gpu ok")

        # Triggers the addition of a CPU node.
        @ray.remote(num_cpus=3)
        def g():
            print("cpu ok")

        ray.get(f.remote())
        ray.get(g.remote())
        # Wait for scale-down.
        wait_for_condition(
            lambda: ray.cluster_resources().get("CPU", 0) == 2, timeout=30
        )
        ray.shutdown()
    finally:
        cluster.shutdown()


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
