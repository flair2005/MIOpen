#include <mlopen.h>
#include "test.hpp"
#include <array>
#include <iterator>
#include <memory>
#include <utility>
#include <iostream>
#include <mlopen/tensor.hpp>
#include <mlopen/pooling.hpp>
#include <limits>

// #include "network_data.hpp"
#include "tensor_holder.hpp"
#include "verify.hpp"
#include "driver.hpp"
#include "get_handle.hpp"

template<class T>
tensor<T> get_output_tensor(const mlopen::PoolingDescriptor& filter, const tensor<T>& input)
{
    return tensor<T>{filter.GetForwardOutputTensor(input.desc)};
}

template<class T>
struct pooling_operators
{
    mlopen::PoolingDescriptor filter;
    pooling_operators(mlopen::PoolingDescriptor f)
    : filter(f)
    {}

    T start() const
    {
        if (filter.GetMode() == mlopenPoolingMax) return std::numeric_limits<T>::lowest();
        else return 0.0;
    }

    T operator()(T x, T y) const
    {
        if (filter.GetMode() == mlopenPoolingMax) return std::max(x, y);
        else return x+y;
    }

    T final(T x, T y)
    {
        if (filter.GetMode() == mlopenPoolingMax) return x;
        else return x / y; 
    }
};

struct verify_forward_pooling
{
    template<class T>
    tensor<T> cpu(const tensor<T>& input, const mlopen::PoolingDescriptor& filter, std::vector<uint16_t>&)
    {
        auto out = get_output_tensor(filter, input);

        int in_h, in_w;
        std::tie(std::ignore, std::ignore, in_h, in_w) = mlopen::tie4(input.desc.GetLengths());

        int u, v, pad_h, pad_w, window_h, window_w;
        std::tie(u, v) = mlopen::tie2(filter.GetStrides());
        std::tie(pad_h, pad_w) = mlopen::tie2(filter.GetPads());
        std::tie(window_h, window_w) = mlopen::tie2(filter.GetLengths());

        auto op = pooling_operators<T>{filter};

        out.par_for_each([&](int o, int w, int i, int j)
        {
            const int start_x = i * v - pad_h;
            const int start_y = j * u - pad_w;

            const int hend = std::min(start_x + window_h, in_h + pad_h);
            const int wend = std::min(start_y + window_w, in_w + pad_w);

            const int pool_size = (hend - start_x) * (wend - start_y);

            T acc = op.start();
            ford(window_h, window_w)([&](int x, int y)
            {
                const int in_x = start_x + x;
                const int in_y = start_y + y;
                if(in_x >= 0 && in_x < in_h && in_y >= 0 && in_y < in_w) {
                    acc = op(acc, input(o, w, in_x, in_y));
                }
            });
            out(o, w, i, j) = op.final(acc, pool_size);
        });
        return out;
    }

    template<class T>
    tensor<T> gpu(const tensor<T>& input, const mlopen::PoolingDescriptor& filter, std::vector<uint16_t>& indices)
    {
        auto&& handle = get_handle();
        auto out = get_output_tensor(filter, input);
        indices.resize(out.data.size(), 0);

        auto in_dev = handle.Write(input.data);
        auto out_dev = handle.Create<T>(out.data.size());
        auto workspace_dev = handle.Write(indices);

        int alpha = 1, beta = 1;
        filter.Forward(
            handle,
            &alpha,
            input.desc,
            in_dev.get(),
            &beta,
            out.desc,
            out_dev.get(),
            true,
            workspace_dev.get(),
            indices.size() * sizeof(uint16_t)
        );

        indices = handle.Read<uint16_t>(workspace_dev, indices.size());
        out.data = handle.Read<T>(out_dev, out.data.size());
        return out;
    }

    template<class T>
    void fail(float, const tensor<T>& input, const mlopen::PoolingDescriptor& filter, const std::vector<uint16_t>&)
    {
        std::cout << "Forward pooling: ";
        if (filter.GetMode() == mlopenPoolingAverage) std::cout << "Average";
        else std::cout << "Max";
        std::cout << std::endl;
        std::cout << "Input tensor: " << input.desc.ToString() << std::endl;
        std::cout << "Output tensor: " << filter.GetForwardOutputTensor(input.desc).ToString() << std::endl;
    }
    
};


struct verify_backward_pooling
{
    template<class T>
    tensor<T> cpu(const tensor<T>& input, const tensor<T>& dout, const tensor<T>& out, const mlopen::PoolingDescriptor& filter, const std::vector<uint16_t>& indices)
    {
        auto dinput = input;
        CHECK(dout.desc == out.desc);
        std::fill(dinput.begin(), dinput.end(), 0.0);

        int in_h, in_w;
        std::tie(std::ignore, std::ignore, in_h, in_w) = mlopen::tie4(dinput.desc.GetLengths());

        int u, v, pad_h, pad_w, window_h, window_w;
        std::tie(u, v) = mlopen::tie2(filter.GetStrides());
        std::tie(pad_h, pad_w) = mlopen::tie2(filter.GetPads());
        std::tie(window_h, window_w) = mlopen::tie2(filter.GetLengths());

        int out_n, out_c, out_h, out_w;
        std::tie(out_n, out_c, out_h, out_w) = mlopen::tie4(out.desc.GetLengths());

        par_ford(out_n, out_c)([&](int o, int w)
        {
            if (filter.GetMode() == mlopenPoolingMax)
            {
                ford(out_h, out_w)([&](int i, int j)
                {
                    auto idx = indices.at(dout.desc.GetIndex(o, w, i, j));
                    auto idx_h = idx / in_w;
                    auto idx_w = idx % in_w;
                    CHECK(float_equal(input(o, w, idx_h, idx_w), out(o, w, i, j)));
                    dinput(o, w, idx_h, idx_w) += dout(o, w, i, j);
                });
            }
            else
            {
                ford(out_h, out_w, window_h, window_w)([&](int i, int j, int x, int y)
                {
                    const int start_x = i * v - pad_h;
                    const int start_y = j * u - pad_w;

                    const int hend = std::min(start_x + window_h, in_h + pad_h);
                    const int wend = std::min(start_y + window_w, in_w + pad_w);

                    const int pool_size = (hend - start_x) * (wend - start_y);

                    const int in_x = start_x + x;
                    const int in_y = start_y + y;
                    if(in_x >= 0 && in_x < in_h && in_y >= 0 && in_y < in_w) {
                        dinput(o, w, in_x, in_y) += dout(o, w, i, j) / pool_size;
                    }
                });
            }
        });
        return dinput;
    }

    template<class T>
    tensor<T> gpu(const tensor<T>& input, const tensor<T>& dout, const tensor<T>& out, const mlopen::PoolingDescriptor& filter, const std::vector<uint16_t>& indices)
    {
        auto&& handle = get_handle();
        auto dinput = input;

        auto in_dev = handle.Write(input.data);
        auto dout_dev = handle.Write(dout.data);
        auto out_dev = handle.Write(out.data);
        auto din_dev = handle.Create<T>(dinput.data.size());

        // std::vector<char> workspace(filter.GetWorkSpaceSize(out.desc));
        // auto workspace_dev = handle.Write(workspace);
        auto workspace_dev = handle.Write(indices);

        int alpha = 1, beta = 1;
        filter.Backward(
            handle,
            &alpha,
            // y
            out.desc,
            out_dev.get(),
            // dy
            dout.desc,
            dout_dev.get(),
            // x
            input.desc,
            in_dev.get(),
            &beta,
            // dx
            dinput.desc,
            din_dev.get(),
            workspace_dev.get()
        );

        dinput.data = handle.Read<T>(din_dev, dinput.data.size());
        return dinput;
    }

    template<class T>
    void fail(float, const tensor<T>& input, const tensor<T>&, const tensor<T>& out, const mlopen::PoolingDescriptor& filter, const std::vector<uint16_t>&)
    {
        std::cout << "Backward pooling: ";
        if (filter.GetMode() == mlopenPoolingAverage) std::cout << "Average";
        else std::cout << "Max";
        std::cout << std::endl;
        std::cout << "Output tensor: " << out.desc.ToString() << std::endl;
        std::cout << "Input tensor: " << input.desc.ToString() << std::endl;
    }
    
};

struct verify_pooling
{
    template<class T>
    void operator()(const tensor<T>& input) const
    {
        int in_h, in_w;
        std::tie(std::ignore, std::ignore, in_h, in_w) = mlopen::tie4(input.desc.GetLengths());
        if ((in_h * in_w) > std::numeric_limits<uint16_t>::max()) return;

        for(auto m:{mlopenPoolingMax, mlopenPoolingAverage})
        {
            for(auto filter:{
                mlopen::PoolingDescriptor{m, {2, 2}, {2, 2}, {0, 0}}, 
                mlopen::PoolingDescriptor{m, {2, 2}, {1, 1}, {0, 0}}, 
                mlopen::PoolingDescriptor{m, {2, 2}, {1, 1}, {1, 1}},
                mlopen::PoolingDescriptor{m, {3, 3}, {2, 2}, {0, 0}}, 
                mlopen::PoolingDescriptor{m, {3, 3}, {1, 1}, {1, 1}}
            })
            {
                std::vector<uint16_t> indices{};
                auto out = verify(verify_forward_pooling{}, input, filter, indices);
                auto dout = out.first;
                dout.generate([&](int n, int c, int h, int w)
                {
                    T x = out.first(n, c, h, w);
                    double y = (877*n+547*c+701*h+1049*w+static_cast<int>(769*x))%2503;
                    return ((x*y)/1301.0);
                });
                verify(verify_backward_pooling{}, input, dout, out.first, filter, indices);
            }
        }
    }
};

int main(int argc, const char *argv[]) 
{
    test_drive<verify_pooling, unary_input>(argc, argv);
}