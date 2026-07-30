#include <vector>
#include <complex>
#include <iostream>
#include <span>
#include <numbers>

using ComplexNum = std::complex<float>;

// Recursive stockham
void Butterfly(size_t len, std::span<ComplexNum> sequence)
{
    if (len == 1)
    {
        return;
    }


    size_t half_len = len / 2;
    float theta = 2.0f * std::numbers::pi_v<float> / len;
    
    for (size_t i = 0; i < half_len; ++i)
    {
        const ComplexNum& curr = ComplexNum{ cos(theta * i), -sin(theta * i) };
        const ComplexNum& even = sequence[i];
        const ComplexNum& odd = sequence[i + half_len];
        sequence[i] = even + odd;
        sequence[i + half_len] = (even - odd) * curr;
    }

    Butterfly(half_len, sequence.subspan(0, half_len));
    Butterfly(half_len, sequence.subspan(half_len, half_len));
}

void BitReversal(std::span<ComplexNum> sequence)
{
    const size_t len = sequence.size();
    for (size_t i = 0, j = 1; j < sequence.size(); ++j)
    {
        // k = len / 2 (len >> 1) [set each bit of i to 1 , bit by bit]
        for (size_t k = len >> 1; k > (i ^= k); k >>= 1)
        {
            if (i < j)
            {
                std::swap(sequence[i], sequence[j]);
            }
        }
    }
}

void BitReversalExpanded(std::span<ComplexNum> sequence)
{
    const size_t N = sequence.size();
    size_t bitReverseOfJ = 0;
    for (size_t j = 1; j < N; ++j)
    {
        // get the bit reversed index for the current iteration j
        size_t bitToFlip = N >> 1;
        do 
        {
            bitReverseOfJ ^= bitToFlip;
        } while (bitToFlip > bitReverseOfJ, bitToFlip >>= 1);

        if (bitReverseOfJ < j)
        {
            std::swap(sequence[bitReverseOfJ], sequence[j]);
        }
    }
}

void CooleyTukeyFFT(std::span<ComplexNum> sequence)
{
    Butterfly(sequence.size(), sequence);
    BitReversal(sequence);
    for (auto& elem : sequence)
    {
        elem /= static_cast<float>(sequence.size());
    }
}

void CooleyTukeyIFFT(std::span<ComplexNum> sequence)
{
    for (auto& elem : sequence)
    {
        elem = std::conj(elem);
    }
    BitReversal(sequence);
    Butterfly(sequence.size(), sequence);
    for (auto& elem : sequence)
    {
        elem = std::conj(elem);
    }
}


int main(int argc, char** argv)
{
    return 0;
}