#include "least_memory_thomas_solver.h"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <omp.h>

#include "solver_utils.h"

template <typename real_t>
void least_memory_thomas_solver<real_t>::precompute_values(std::unique_ptr<real_t[]>& a, std::unique_ptr<real_t[]>& b0,
														   std::unique_ptr<index_t[]>& threshold_index, index_t shape,
														   index_t dims, index_t n)
{
	a = std::make_unique<real_t[]>(problem_.substrates_count);
	b0 = std::make_unique<real_t[]>(problem_.substrates_count);
	threshold_index = std::make_unique<index_t[]>(problem_.substrates_count);

	// compute a_i, b0_i
	for (index_t s = 0; s < problem_.substrates_count; s++)
	{
		a[s] = -problem_.dt * problem_.diffusion_coefficients[s] / (shape * shape);
		b0[s] = 1 + problem_.dt * problem_.decay_rates[s] / dims
				+ problem_.dt * problem_.diffusion_coefficients[s] / (shape * shape);
	}

	real_t prev, curr;
	for (index_t s = 0; s < problem_.substrates_count; s++)
	{
		for (index_t i = 0; i < n; i++)
		{
			// computes one element
			{
				if (i == 0)
					curr = b0[s];
				else if (i != n - 1)
				{
					prev = curr;
					curr = (b0[s] - a[s]) - (a[s] * a[s]) / prev;
				}
				else
				{
					prev = curr;
					curr = b0[s] - (a[s] * a[s]) / prev;
				}
			}

			if (i > 0 && std::abs(curr - prev) < limit_threshold_)
			{
				threshold_index[s] = i;
				break;
			}
			else if (i == n - 1)
			{
				threshold_index[s] = n;
			}
		}
	}
}

template <typename real_t>
void least_memory_thomas_solver<real_t>::prepare(const max_problem_t& problem)
{
	problem_ = problems::cast<std::int32_t, real_t>(problem);
	substrates_ = std::make_unique<real_t[]>(problem_.nx * problem_.ny * problem_.nz * problem_.substrates_count);

	// Initialize substrates

	auto substrates_layout = get_substrates_layout<3>(problem_);

	solver_utils::initialize_substrate(substrates_layout, substrates_.get(), problem_);
}

template <typename real_t>
void least_memory_thomas_solver<real_t>::tune(const nlohmann::json& params)
{
	work_items_ = params.contains("work_items") ? (std::size_t)params["work_items"] : 1;
}

template <typename real_t>
void least_memory_thomas_solver<real_t>::initialize()
{
	if (problem_.dims >= 1)
		precompute_values(ax_, b0x_, threshold_indexx_, problem_.dx, problem_.dims, problem_.nx);
	if (problem_.dims >= 2)
		precompute_values(ay_, b0y_, threshold_indexy_, problem_.dy, problem_.dims, problem_.ny);
	if (problem_.dims >= 3)
		precompute_values(az_, b0z_, threshold_indexz_, problem_.dz, problem_.dims, problem_.nz);
}

template <typename real_t>
template <std::size_t dims>
auto least_memory_thomas_solver<real_t>::get_substrates_layout(const problem_t<index_t, real_t>& problem)
{
	if constexpr (dims == 1)
		return noarr::scalar<real_t>() ^ noarr::vectors<'x', 's'>(problem.nx, problem.substrates_count);
	else if constexpr (dims == 2)
		return noarr::scalar<real_t>()
			   ^ noarr::vectors<'x', 'y', 's'>(problem.nx, problem.ny, problem.substrates_count);
	else if constexpr (dims == 3)
		return noarr::scalar<real_t>()
			   ^ noarr::vectors<'x', 'y', 'z', 's'>(problem.nx, problem.ny, problem.nz, problem.substrates_count);
}

template <typename index_t, typename real_t, typename density_layout_t>
void solve_slice_x_1d(real_t* __restrict__ densities, const real_t* __restrict__ a, const real_t* __restrict__ b0,
					  const index_t* __restrict__ threshold, const density_layout_t dens_l, std::size_t work_items)
{
	const index_t substrates_count = dens_l | noarr::get_length<'s'>();
	const index_t n = dens_l | noarr::get_length<'x'>();

#pragma omp for schedule(static, work_items) nowait
	for (index_t s = 0; s < substrates_count; s++)
	{
		real_t b_tmp = b0[s];

		{
			(dens_l | noarr::get_at<'x', 's'>(densities, 1, s)) -=
				a[s] * (dens_l | noarr::get_at<'x', 's'>(densities, 0, s)) / b_tmp;

			// std::cout << "-ftmp 0: " << b_tmp << std::endl;
		}

		for (index_t i = 2; i < threshold[s]; i++)
		{
			b_tmp = (b0[s] - a[s]) - (a[s] * a[s]) / b_tmp;

			(dens_l | noarr::get_at<'x', 's'>(densities, i, s)) -=
				a[s] * (dens_l | noarr::get_at<'x', 's'>(densities, i - 1, s)) / b_tmp;


			// std::cout << "-ftmp " << i - 1 << ": " << b_tmp << std::endl;
		}

		for (index_t i = threshold[s]; i < n; i++)
		{
			(dens_l | noarr::get_at<'x', 's'>(densities, i, s)) -=
				a[s] * (dens_l | noarr::get_at<'x', 's'>(densities, i - 1, s)) / b_tmp;

			// std::cout << "ftmp " << i - 1 << ": " << b_tmp << std::endl;
		}

		{
			(dens_l | noarr::get_at<'x', 's'>(densities, n - 1, s)) /= b0[s] - (a[s] * a[s]) / b_tmp;

			// std::cout << "ftmp " << n - 1 << ": " << b0[s] - (a[s] * a[s]) / b_tmp << std::endl;
		}

		{
			(dens_l | noarr::get_at<'x', 's'>(densities, n - 2, s)) =
				((dens_l | noarr::get_at<'x', 's'>(densities, n - 2, s))
				 - a[s] * (dens_l | noarr::get_at<'x', 's'>(densities, n - 1, s)))
				/ b_tmp;

			// std::cout << "ftmp " << n - 2 << ": " << b_tmp << std::endl;
		}

		for (index_t i = n - 3; i >= threshold[s] - 1; i--)
		{
			(dens_l | noarr::get_at<'x', 's'>(densities, i, s)) =
				((dens_l | noarr::get_at<'x', 's'>(densities, i, s))
				 - a[s] * (dens_l | noarr::get_at<'x', 's'>(densities, i + 1, s)))
				/ b_tmp;

			// std::cout << "btmp " << i << ": " << b_tmp << std::endl;
		}

		for (index_t i = threshold[s] - 2; i >= 0; i--)
		{
			(dens_l | noarr::get_at<'x', 's'>(densities, i, s)) =
				((dens_l | noarr::get_at<'x', 's'>(densities, i, s))
				 - a[s] * (dens_l | noarr::get_at<'x', 's'>(densities, i + 1, s)))
				/ b_tmp;

			// std::cout << "-btmp " << i << ": " << b_tmp << std::endl;

			b_tmp = (a[s] * a[s]) / (b0[s] - a[s] - b_tmp);
		}
	}
}

template <typename index_t, typename real_t, typename density_layout_t>
void solve_slice_x_2d_and_3d(real_t* __restrict__ densities, const real_t* __restrict__ a,
							 const real_t* __restrict__ b0, const index_t* __restrict__ threshold,
							 const density_layout_t dens_l, std::size_t work_items)
{
	const index_t substrates_count = dens_l | noarr::get_length<'s'>();
	const index_t n = dens_l | noarr::get_length<'x'>();
	const index_t m = dens_l | noarr::get_length<'m'>();

#pragma omp for schedule(static, work_items) collapse(2) nowait
	for (index_t s = 0; s < substrates_count; s++)
	{
		for (index_t yz = 0; yz < m; yz++)
		{
			real_t b_tmp = b0[s];

			{
				(dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, 1, s)) -=
					a[s] * (dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, 0, s)) / b_tmp;
			}

			for (index_t i = 2; i < threshold[s]; i++)
			{
				b_tmp = (b0[s] - a[s]) - (a[s] * a[s]) / b_tmp;

				(dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, i, s)) -=
					a[s] * (dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, i - 1, s)) / b_tmp;
			}

			for (index_t i = threshold[s]; i < n; i++)
			{
				(dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, i, s)) -=
					a[s] * (dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, i - 1, s)) / b_tmp;
			}

			{
				(dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, n - 1, s)) /= b0[s] - (a[s] * a[s]) / b_tmp;
			}

			{
				(dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, n - 2, s)) =
					((dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, n - 2, s))
					 - a[s] * (dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, n - 1, s)))
					/ b_tmp;
			}

			for (index_t i = n - 3; i >= threshold[s] - 1; i--)
			{
				(dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, i, s)) =
					((dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, i, s))
					 - a[s] * (dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, i + 1, s)))
					/ b_tmp;
			}

			for (index_t i = threshold[s] - 2; i >= 0; i--)
			{
				(dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, i, s)) =
					((dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, i, s))
					 - a[s] * (dens_l | noarr::get_at<'m', 'x', 's'>(densities, yz, i + 1, s)))
					/ b_tmp;

				b_tmp = (a[s] * a[s]) / (b0[s] - a[s] - b_tmp);
			}
		}
	}
}

template <typename index_t, typename real_t, typename density_layout_t>
void solve_slice_y_2d(real_t* __restrict__ densities, const real_t* __restrict__ a, const real_t* __restrict__ b0,
					  const index_t* __restrict__ threshold, const density_layout_t dens_l, std::size_t work_items)
{
	const index_t substrates_count = dens_l | noarr::get_length<'s'>();
	const index_t n = dens_l | noarr::get_length<'y'>();
	const index_t x_len = dens_l | noarr::get_length<'x'>();

#pragma omp for schedule(static, work_items) nowait
	for (index_t s = 0; s < substrates_count; s++)
	{
		real_t b_tmp = b0[s];

#pragma omp simd
		for (index_t x = 0; x < x_len; x++)
		{
			(dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, 1, s)) -=
				a[s] * (dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, 0, s)) / b_tmp;
		}

		for (index_t i = 2; i < threshold[s]; i++)
		{
			b_tmp = (b0[s] - a[s]) - (a[s] * a[s]) / b_tmp;

#pragma omp simd
			for (index_t x = 0; x < x_len; x++)
			{
				(dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, i, s)) -=
					a[s] * (dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, i - 1, s)) / b_tmp;
			}
		}

		for (index_t i = threshold[s]; i < n; i++)
		{
#pragma omp simd
			for (index_t x = 0; x < x_len; x++)
			{
				(dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, i, s)) -=
					a[s] * (dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, i - 1, s)) / b_tmp;
			}
		}

#pragma omp simd
		for (index_t x = 0; x < x_len; x++)
		{
			(dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, n - 1, s)) /= b0[s] - (a[s] * a[s]) / b_tmp;

			(dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, n - 2, s)) =
				((dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, n - 2, s))
				 - a[s] * (dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, n - 1, s)))
				/ b_tmp;
		}

		for (index_t i = n - 3; i >= threshold[s] - 1; i--)
		{
#pragma omp simd
			for (index_t x = 0; x < x_len; x++)
			{
				(dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, i, s)) =
					((dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, i, s))
					 - a[s] * (dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, i + 1, s)))
					/ b_tmp;
			}
		}

		for (index_t i = threshold[s] - 2; i >= 0; i--)
		{
#pragma omp simd
			for (index_t x = 0; x < x_len; x++)
			{
				(dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, i, s)) =
					((dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, i, s))
					 - a[s] * (dens_l | noarr::get_at<'x', 'y', 's'>(densities, x, i + 1, s)))
					/ b_tmp;
			}

			b_tmp = (a[s] * a[s]) / (b0[s] - a[s] - b_tmp);
		}
	}
}

template <typename index_t, typename real_t, typename density_layout_t>
void solve_slice_y_3d(real_t* __restrict__ densities, const real_t* __restrict__ a, const real_t* __restrict__ b0,
					  const index_t* __restrict__ threshold, const density_layout_t dens_l, std::size_t work_items)
{
	const index_t substrates_count = dens_l | noarr::get_length<'s'>();
	const index_t n = dens_l | noarr::get_length<'y'>();
	const index_t z_len = dens_l | noarr::get_length<'z'>();
	const index_t x_len = dens_l | noarr::get_length<'x'>();

#pragma omp for schedule(static, work_items) collapse(2) nowait
	for (index_t s = 0; s < substrates_count; s++)
	{
		for (index_t z = 0; z < z_len; z++)
		{
			real_t b_tmp = b0[s];

#pragma omp simd
			for (index_t x = 0; x < x_len; x++)
			{
				(dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, 1, s)) -=
					a[s] * (dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, 0, s)) / b_tmp;
			}

			for (index_t i = 2; i < threshold[s]; i++)
			{
				b_tmp = (b0[s] - a[s]) - (a[s] * a[s]) / b_tmp;

#pragma omp simd
				for (index_t x = 0; x < x_len; x++)
				{
					(dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, i, s)) -=
						a[s] * (dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, i - 1, s)) / b_tmp;
				}
			}

			for (index_t i = threshold[s]; i < n; i++)
			{
#pragma omp simd
				for (index_t x = 0; x < x_len; x++)
				{
					(dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, i, s)) -=
						a[s] * (dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, i - 1, s)) / b_tmp;
				}
			}

#pragma omp simd
			for (index_t x = 0; x < x_len; x++)
			{
				(dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, n - 1, s)) /=
					b0[s] - (a[s] * a[s]) / b_tmp;

				(dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, n - 2, s)) =
					((dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, n - 2, s))
					 - a[s] * (dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, n - 1, s)))
					/ b_tmp;
			}

			for (index_t i = n - 3; i >= threshold[s] - 1; i--)
			{
#pragma omp simd
				for (index_t x = 0; x < x_len; x++)
				{
					(dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, i, s)) =
						((dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, i, s))
						 - a[s] * (dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, i + 1, s)))
						/ b_tmp;
				}
			}

			for (index_t i = threshold[s] - 2; i >= 0; i--)
			{
#pragma omp simd
				for (index_t x = 0; x < x_len; x++)
				{
					(dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, i, s)) =
						((dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, i, s))
						 - a[s] * (dens_l | noarr::get_at<'z', 'x', 'y', 's'>(densities, z, x, i + 1, s)))
						/ b_tmp;
				}

				b_tmp = (a[s] * a[s]) / (b0[s] - a[s] - b_tmp);
			}
		}
	}
}

template <typename index_t, typename real_t, typename density_layout_t>
void solve_slice_z_3d(real_t* __restrict__ densities, const real_t* __restrict__ a, const real_t* __restrict__ b0,
					  const index_t* __restrict__ threshold, const density_layout_t dens_l, std::size_t work_items)
{
	const index_t substrates_count = dens_l | noarr::get_length<'s'>();
	const index_t n = dens_l | noarr::get_length<'z'>();
	const index_t y_len = dens_l | noarr::get_length<'y'>();
	const index_t x_len = dens_l | noarr::get_length<'x'>();

#pragma omp for schedule(static, work_items) nowait
	for (index_t s = 0; s < substrates_count; s++)
	{
		real_t b_tmp = b0[s];

		for (index_t y = 0; y < y_len; y++)
		{
#pragma omp simd
			for (index_t x = 0; x < x_len; x++)
			{
				(dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, 1, s)) -=
					a[s] * (dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, 0, s)) / b_tmp;
			}
		}

		for (index_t i = 2; i < threshold[s]; i++)
		{
			b_tmp = (b0[s] - a[s]) - (a[s] * a[s]) / b_tmp;

			for (index_t y = 0; y < y_len; y++)
			{
#pragma omp simd
				for (index_t x = 0; x < x_len; x++)
				{
					(dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, i, s)) -=
						a[s] * (dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, i - 1, s)) / b_tmp;
				}
			}
		}

		for (index_t i = threshold[s]; i < n; i++)
		{
			for (index_t y = 0; y < y_len; y++)
			{
#pragma omp simd
				for (index_t x = 0; x < x_len; x++)
				{
					(dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, i, s)) -=
						a[s] * (dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, i - 1, s)) / b_tmp;
				}
			}
		}

		for (index_t y = 0; y < y_len; y++)
		{
#pragma omp simd
			for (index_t x = 0; x < x_len; x++)
			{
				(dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, n - 1, s)) /=
					b0[s] - (a[s] * a[s]) / b_tmp;

				(dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, n - 2, s)) =
					((dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, n - 2, s))
					 - a[s] * (dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, n - 1, s)))
					/ b_tmp;
			}
		}

		for (index_t i = n - 3; i >= threshold[s] - 1; i--)
		{
			for (index_t y = 0; y < y_len; y++)
			{
#pragma omp simd
				for (index_t x = 0; x < x_len; x++)
				{
					(dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, i, s)) =
						((dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, i, s))
						 - a[s] * (dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, i + 1, s)))
						/ b_tmp;
				}
			}
		}

		for (index_t i = threshold[s] - 2; i >= 0; i--)
		{
			for (index_t y = 0; y < y_len; y++)
			{
#pragma omp simd
				for (index_t x = 0; x < x_len; x++)
				{
					(dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, i, s)) =
						((dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, i, s))
						 - a[s] * (dens_l | noarr::get_at<'x', 'y', 'z', 's'>(densities, x, y, i + 1, s)))
						/ b_tmp;
				}
			}

			b_tmp = (a[s] * a[s]) / (b0[s] - a[s] - b_tmp);
		}
	}
}

template <typename real_t>
void least_memory_thomas_solver<real_t>::solve_x()
{
	if (problem_.dims == 1)
	{
#pragma omp parallel
		solve_slice_x_1d<index_t>(substrates_.get(), ax_.get(), b0x_.get(), threshold_indexx_.get(),
								  get_substrates_layout<1>(problem_), work_items_);
	}
	else if (problem_.dims == 2)
	{
#pragma omp parallel
		solve_slice_x_2d_and_3d<index_t>(substrates_.get(), ax_.get(), b0x_.get(), threshold_indexx_.get(),
										 get_substrates_layout<2>(problem_) ^ noarr::rename<'y', 'm'>(), work_items_);
	}
	else if (problem_.dims == 3)
	{
#pragma omp parallel
		solve_slice_x_2d_and_3d<index_t>(substrates_.get(), ax_.get(), b0x_.get(), threshold_indexx_.get(),
										 get_substrates_layout<3>(problem_) ^ noarr::merge_blocks<'z', 'y', 'm'>(),
										 work_items_);
	}
}

template <typename real_t>
void least_memory_thomas_solver<real_t>::solve_y()
{
	if (problem_.dims == 2)
	{
#pragma omp parallel
		solve_slice_y_2d<index_t>(substrates_.get(), ay_.get(), b0y_.get(), threshold_indexy_.get(),
								  get_substrates_layout<2>(problem_), work_items_);
	}
	else if (problem_.dims == 3)
	{
#pragma omp parallel
		solve_slice_y_3d<index_t>(substrates_.get(), ay_.get(), b0y_.get(), threshold_indexy_.get(),
								  get_substrates_layout<3>(problem_), work_items_);
	}
}

template <typename real_t>
void least_memory_thomas_solver<real_t>::solve_z()
{
#pragma omp parallel
	solve_slice_z_3d<index_t>(substrates_.get(), az_.get(), b0z_.get(), threshold_indexz_.get(),
							  get_substrates_layout<3>(problem_), work_items_);
}

template <typename real_t>
void least_memory_thomas_solver<real_t>::save(const std::string& file) const
{
	auto dens_l = get_substrates_layout<3>(problem_);

	std::ofstream out(file);

	for (index_t z = 0; z < problem_.nz; z++)
		for (index_t y = 0; y < problem_.ny; y++)
			for (index_t x = 0; x < problem_.nx; x++)
			{
				for (index_t s = 0; s < problem_.substrates_count; s++)
					out << (dens_l | noarr::get_at<'s', 'x', 'y', 'z'>(substrates_.get(), s, x, y, z)) << " ";
				out << std::endl;
			}

	out.close();
}

template <typename real_t>
double least_memory_thomas_solver<real_t>::access(std::size_t s, std::size_t x, std::size_t y, std::size_t z) const
{
	auto dens_l = get_substrates_layout<3>(problem_);

	return (dens_l | noarr::get_at<'s', 'x', 'y', 'z'>(substrates_.get(), s, x, y, z));
}

template <>
float least_memory_thomas_solver<float>::limit_threshold_ = 1e-6f;

template <>
double least_memory_thomas_solver<double>::limit_threshold_ = 1e-12;

template class least_memory_thomas_solver<float>;
template class least_memory_thomas_solver<double>;
