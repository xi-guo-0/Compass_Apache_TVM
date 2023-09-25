# This file is CONFIDENTIAL and created by Arm Technology (China) Co., Ltd.
# See the copyright file distributed with this work for additional information
# regarding copyright ownership.
import pytest
import tensorflow as tf
from tvm.relay.backend.contrib.aipu_compass import testing as aipu_testing


def eltwise(input0, input1, method):
    if method.upper() == "ADD":
        return tf.add(input0, input1)
    elif method.upper() == "SUB":
        return tf.subtract(input0, input1)
    elif method.upper() == "MUL":
        return tf.multiply(input0, input1)
    else:
        raise NotImplementedError(f"unsupport method: {method}")


@pytest.mark.parametrize(
    "method",
    [
        "Add",
        "Mul",
        "Sub",
    ],
)
@pytest.mark.parametrize(
    "input_shapes",
    [
        ([[5, 10], [5, 10]]),
        ([[5, 10, 224], [5, 10, 224]]),
        ([[5, 10, 224, 100], [5, 10, 224, 100]]),
    ],
)
def test_eltwiserelu(method, input_shapes):
    op_type = "EltwiseRelu"
    dim_info = f"{len(input_shapes[0])}d"
    model_name = aipu_testing.gen_model_name(op_type, dim_info, method)

    g = tf.Graph()
    with g.as_default():
        inputs = aipu_testing.get_input_tensor_of_tf(input_shapes)
        inp = inputs[0]
        inp2 = inputs[1]
        out_eltwise = eltwise(inp, inp2, method)
        out = tf.nn.relu(out_eltwise)

    model_info = {
        "model_name": model_name,
        "op_type": op_type,
        "input_shapes": input_shapes,
        "inputs": inputs,
        "outputs": [out],
        "in_graph": g,
    }

    cfg_file = aipu_testing.get_model_cfg_path(model_info, "tf")
    input_data = aipu_testing.get_op_input(model_name, input_shapes)
    aipu_output = aipu_testing.get_tvm_output(cfg_file, input_data)
    aipu_testing.get_test_result(aipu_testing.TFModel(cfg_file), input_data, aipu_output, 0.99)
