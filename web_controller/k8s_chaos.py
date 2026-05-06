from __future__ import annotations

import json
import shlex
from typing import Any, Dict, List


DEFAULT_NAMESPACE = "default"
DEFAULT_APP_LABEL_KEY = "app"
DEFAULT_APP_LABEL_VALUE = "nginx-demo"
NETWORK_PROBE_LABEL_KEY = "app"
NETWORK_PROBE_LABEL_VALUE = "fi-net-probe"


def _kubectl(ctx: Dict[str, Any]) -> str:
    kubectl_cmd = str(ctx.get("kubectl_cmd") or "kubectl").strip()
    kubeconfig = str(ctx.get("kubeconfig") or "").strip()
    if kubeconfig:
        return f"KUBECONFIG={shlex.quote(kubeconfig)} {kubectl_cmd}"
    return kubectl_cmd


def _shell_cmd(script: str) -> List[str]:
    return ["/bin/sh", "-lc", script]


def _duration(params: Dict[str, Any]) -> str:
    return f"{int(params.get('duration', 120))}s"


def _selector(params: Dict[str, Any]) -> Dict[str, Any]:
    namespace = str(params.get("namespace") or DEFAULT_NAMESPACE)
    label_key = str(params.get("label_key") or DEFAULT_APP_LABEL_KEY)
    label_value = str(params.get("label_value") or DEFAULT_APP_LABEL_VALUE)
    return {
        "namespaces": [namespace],
        "labelSelectors": {
            label_key: label_value,
        },
    }


def _network_probe_target(params: Dict[str, Any]) -> Dict[str, Any]:
    namespace = str(params.get("namespace") or DEFAULT_NAMESPACE)
    return {
        "mode": "all",
        "selector": {
            "namespaces": [namespace],
            "labelSelectors": {
                NETWORK_PROBE_LABEL_KEY: NETWORK_PROBE_LABEL_VALUE,
            },
        },
    }


def _metadata(params: Dict[str, Any], default_name: str) -> Dict[str, str]:
    return {
        "name": str(params.get("chaos_name") or default_name),
        "namespace": str(params.get("namespace") or DEFAULT_NAMESPACE),
    }


def _manifest_apply_cmd(ctx: Dict[str, Any], manifest: Dict[str, Any]) -> List[str]:
    manifest_json = json.dumps(manifest, ensure_ascii=False, separators=(",", ":"))
    kubectl = _kubectl(ctx)
    script = f"printf %s {shlex.quote(manifest_json)} | {kubectl} apply -f -"
    return _shell_cmd(script)


def k8s_status_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    kubectl = _kubectl(ctx)
    namespace = shlex.quote(str(params.get("namespace") or DEFAULT_NAMESPACE))
    script = (
        f"{kubectl} get nodes -o wide; "
        f"{kubectl} get pods -A -o wide; "
        f"{kubectl} get podchaos,networkchaos,stresschaos -A 2>/dev/null || true; "
        f"{kubectl} get events -n {namespace} --sort-by=.lastTimestamp | tail -n 30 || true"
    )
    return [_shell_cmd(script)]


def k8s_demo_deploy_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    kubectl = _kubectl(ctx)
    namespace = shlex.quote(str(params.get("namespace") or DEFAULT_NAMESPACE))
    name = shlex.quote(str(params.get("deployment") or DEFAULT_APP_LABEL_VALUE))
    image = shlex.quote(str(params.get("image") or "nginx"))
    replicas = int(params.get("replicas", 2))
    script = (
        f"{kubectl} create deployment {name} -n {namespace} --image={image} "
        f"--replicas={replicas} --dry-run=client -o yaml | {kubectl} apply -f -; "
        f"{kubectl} rollout status deployment/{name} -n {namespace} --timeout=180s; "
        f"{kubectl} get pods -n {namespace} -l app={name} -o wide"
    )
    return [_shell_cmd(script)]


def k8s_demo_delete_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    kubectl = _kubectl(ctx)
    namespace = shlex.quote(str(params.get("namespace") or DEFAULT_NAMESPACE))
    name = shlex.quote(str(params.get("deployment") or DEFAULT_APP_LABEL_VALUE))
    script = f"{kubectl} delete deployment/{name} -n {namespace} --ignore-not-found"
    return [_shell_cmd(script)]


def pod_kill_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    manifest = {
        "apiVersion": "chaos-mesh.org/v1alpha1",
        "kind": "PodChaos",
        "metadata": _metadata(params, "fi-pod-kill"),
        "spec": {
            "action": "pod-kill",
            "mode": str(params.get("chaos_mode") or "one"),
            "selector": _selector(params),
        },
    }
    return [_manifest_apply_cmd(ctx, manifest)]


def container_kill_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    spec: Dict[str, Any] = {
        "action": "container-kill",
        "mode": str(params.get("chaos_mode") or "one"),
        "selector": _selector(params),
    }
    container_name = str(params.get("container_name") or "").strip()
    if container_name:
        spec["containerNames"] = [container_name]
    manifest = {
        "apiVersion": "chaos-mesh.org/v1alpha1",
        "kind": "PodChaos",
        "metadata": _metadata(params, "fi-container-kill"),
        "spec": spec,
    }
    return [_manifest_apply_cmd(ctx, manifest)]


def network_delay_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    latency = f"{int(params.get('ms', 200))}ms"
    jitter = f"{int(params.get('jitter', 50))}ms"
    correlation = str(int(params.get("correlation", 25)))
    manifest = {
        "apiVersion": "chaos-mesh.org/v1alpha1",
        "kind": "NetworkChaos",
        "metadata": _metadata(params, "fi-network-delay"),
        "spec": {
            "action": "delay",
            "mode": str(params.get("chaos_mode") or "all"),
            "selector": _selector(params),
            "delay": {
                "latency": latency,
                "jitter": jitter,
                "correlation": correlation,
            },
            "direction": "both",
            "target": _network_probe_target(params),
            "duration": _duration(params),
        },
    }
    return [_manifest_apply_cmd(ctx, manifest)]


def network_loss_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    loss = str(int(params.get("percent", 10)))
    correlation = str(int(params.get("correlation", 25)))
    manifest = {
        "apiVersion": "chaos-mesh.org/v1alpha1",
        "kind": "NetworkChaos",
        "metadata": _metadata(params, "fi-network-loss"),
        "spec": {
            "action": "loss",
            "mode": str(params.get("chaos_mode") or "all"),
            "selector": _selector(params),
            "loss": {
                "loss": loss,
                "correlation": correlation,
            },
            "direction": "both",
            "target": _network_probe_target(params),
            "duration": _duration(params),
        },
    }
    return [_manifest_apply_cmd(ctx, manifest)]


def cpu_stress_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    manifest = {
        "apiVersion": "chaos-mesh.org/v1alpha1",
        "kind": "StressChaos",
        "metadata": _metadata(params, "fi-cpu-stress"),
        "spec": {
            "mode": str(params.get("chaos_mode") or "one"),
            "selector": _selector(params),
            "stressors": {
                "cpu": {
                    "workers": int(params.get("workers", 2)),
                    "load": int(params.get("load", 80)),
                },
            },
            "duration": _duration(params),
        },
    }
    return [_manifest_apply_cmd(ctx, manifest)]


def memory_stress_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    manifest = {
        "apiVersion": "chaos-mesh.org/v1alpha1",
        "kind": "StressChaos",
        "metadata": _metadata(params, "fi-memory-stress"),
        "spec": {
            "mode": str(params.get("chaos_mode") or "one"),
            "selector": _selector(params),
            "stressors": {
                "memory": {
                    "workers": int(params.get("workers", 1)),
                    "size": f"{int(params.get('memory_mb', 256))}MB",
                },
            },
            "duration": _duration(params),
        },
    }
    return [_manifest_apply_cmd(ctx, manifest)]


def chaos_status_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    kubectl = _kubectl(ctx)
    namespace = shlex.quote(str(params.get("namespace") or DEFAULT_NAMESPACE))
    chaos_name = str(params.get("chaos_name") or "").strip()
    if chaos_name:
        quoted_name = shlex.quote(chaos_name)
        script = (
            f"{kubectl} describe podchaos,networkchaos,stresschaos {quoted_name} -n {namespace} "
            f"2>/dev/null || {kubectl} get podchaos,networkchaos,stresschaos -n {namespace}"
        )
    else:
        script = (
            f"{kubectl} get podchaos,networkchaos,stresschaos -A; "
            f"{kubectl} get events -n {namespace} --sort-by=.lastTimestamp | tail -n 40"
        )
    return [_shell_cmd(script)]


def chaos_clear_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    kubectl = _kubectl(ctx)
    namespace = shlex.quote(str(params.get("namespace") or DEFAULT_NAMESPACE))
    kind = str(params.get("chaos_kind") or "all")
    chaos_name = str(params.get("chaos_name") or "").strip()
    if kind == "all":
        script = (
            f"for kind in podchaos networkchaos stresschaos; do "
            f"{kubectl} delete \"$kind\" --all -n {namespace} --ignore-not-found; "
            "done"
        )
    elif chaos_name:
        script = f"{kubectl} delete {shlex.quote(kind)} {shlex.quote(chaos_name)} -n {namespace} --ignore-not-found"
    else:
        script = f"{kubectl} delete {shlex.quote(kind)} --all -n {namespace} --ignore-not-found"
    return [_shell_cmd(script)]
