import pytest

from dicp.vendor.AscendGraph import ext_ops
from ..common.utils import (
    torch,
    dynamo,
    parse_args,
    compile_model,
    get_device,
    Size,
    update_dynamo_config,
)


class OpModule(torch.nn.Module):
    def forward(self, x, cos, sin):
        res = torch.ops.lightllm.rotary_emb.default(x, cos, sin)
        return res


model = OpModule()
args = parse_args()
compiled_model = compile_model(model, args.backend, args.dynamic)


class TestLightllmRotaryEmb():
    @pytest.mark.parametrize("dtype", [torch.float32])
    @pytest.mark.parametrize("sizes", [Size(((2, 32, 64), (2, 32), (2, 32)), ((2, 32, 64), (2, 32), (2, 32))), Size(((2, 32, 128), (2, 64), (2, 64)), ((2, 32, 128), (2, 64), (2, 64)))])
    @pytest.mark.parametrize("compiled_model", compiled_model)
    def test_lightllm_rotary_emb(self, sizes, dtype, compiled_model):
        device = get_device()
        size = sizes.dynamic if compiled_model.dynamic else sizes.static
        input1 = torch.randn(size[0], dtype=dtype)
        input2 = torch.randn(size[1], dtype=dtype)
        input3 = torch.randn(size[2], dtype=dtype)

        dicp_input1 = input1.to(device)
        dicp_input2 = input2.to(device)
        dicp_input3 = input3.to(device)

        output = model(input1, input2, input3)
        dynamo.reset()
        update_dynamo_config(compiled_model.dynamic)
        dicp_output = compiled_model.model(dicp_input1, dicp_input2, dicp_input3)

        assert torch.allclose(output, dicp_output.cpu(), rtol=1e-02, atol=1e-02, equal_nan=True)
