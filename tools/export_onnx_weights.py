# Copyright (c) 2026 AydoganCan60- MIT License
import argparse
import json
import struct
from pathlib import Path

import numpy as np
import onnx
from onnx import numpy_helper


def main() -> None:
    parser = argparse.ArgumentParser(description="Export ONNX initializers for the DirectCompute preview runtime")
    parser.add_argument("model", type=Path)
    parser.add_argument("--weights", type=Path, default=Path("models/espcn_x3.weights"))
    parser.add_argument("--graph", type=Path, default=Path("models/espcn_x3.graph.json"))
    args = parser.parse_args()

    model = onnx.load(args.model)
    tensors = []
    values = []
    offset = 0
    for initializer in model.graph.initializer:
        array = np.asarray(numpy_helper.to_array(initializer), dtype=np.float32).reshape(-1)
        tensors.append({"name": initializer.name, "shape": list(initializer.dims), "offset": offset, "count": int(array.size)})
        values.append(array)
        offset += int(array.size)

    packed = np.concatenate(values) if values else np.empty(0, dtype=np.float32)
    args.weights.parent.mkdir(parents=True, exist_ok=True)
    with args.weights.open("wb") as output:
        output.write(struct.pack("<4sIII", b"NWEI", 1, packed.size, len(tensors)))
        output.write(packed.astype("<f4", copy=False).tobytes())

    graph = {
        "format": "dlssforamd.graph.v1",
        "source": str(args.model),
        "opset": [entry.version for entry in model.opset_import],
        "inputs": [value.name for value in model.graph.input],
        "outputs": [value.name for value in model.graph.output],
        "nodes": [{"name": node.name, "op": node.op_type, "inputs": list(node.input), "outputs": list(node.output)} for node in model.graph.node],
        "tensors": tensors,
    }
    args.graph.write_text(json.dumps(graph, indent=2), encoding="utf-8")
    print(f"exported {packed.size} float32 weights from {len(tensors)} tensors")


if __name__ == "__main__":
    main()
