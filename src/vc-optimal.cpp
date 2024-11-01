/* This file is part of vcube.
 *
 * Copyright (C) 2018 Andrew Skalski
 *
 * vcube is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vcube is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vcube.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <cstdio>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>
#include <string>
#include <utility>
#include <algorithm>
#include <deque>
#include <getopt.h>
#include <libgen.h>
#include <sys/resource.h>
#include "nxprune.h"
#include "nxprune_generator.h"
#include "nxsolve.h"

using namespace vcube;

enum format_t {
	FMT_MOVES,
	FMT_REID,
	FMT_SPEFFZ,
};

/* Configuration */
static struct {
	std::string path;
	uint32_t workers;
	uint32_t coord;
	moveseq_t::style_t style;
	std::array<int, 2> speffz_buffer;
	format_t format;
	bool no_input;
	bool shm;
	bool inverse;
} cf;

static std::string base_path(const char *argv0);

template<nx::EPvariant EP, nx::EOvariant EO, int Base>
static void solver(const std::string &table_filename, uint32_t shm_key);
static void usage(const char *argv0, int status = EXIT_FAILURE);

struct solver_variant {
	int id;
	void (*func)(const std::string &, uint32_t shm_key);
	std::string filename;
	uint32_t shm_key;
	size_t size;

	bool operator < (const solver_variant &o) const {
		return size < o.size;
	}

	void operator()() {
		func(filename, shm_key);
	}

	template<nx::EPvariant EP, nx::EOvariant EO, int Base>
	static solver_variant S(int id) {
		char filename[64];
		sprintf(filename, "tables/nxprune_%d_%02d_%02d.dat",
				EP + 1, (EO + 1) * 4, Base);
		uint32_t shm_key = 0x76630000 |
			(Base << 8) |
			((EP + 1) << 4) |
			((EO + 1) * 4);
		return {
			id,
			solver<EP, EO, Base>,
			filename,
			shm_key,
			nx::prune<nx::ecoord<EP, EO>, Base>().size()
		};
	}
};

/* Some variants are commented out, either because they do not yet have
 * a base value chosen (510 GiB and 4 TiB), or because they are too slow
 * and unnecessarily increase code size by instantiting templates that
 * will see little use.
 */
static std::vector<solver_variant> solvers = {
	//solver_variant::S<nx::EP1, nx::EO4,   7>(104),
	//solver_variant::S<nx::EP1, nx::EO8,   8>(108),
	solver_variant::S<nx::EP1, nx::EO12,  9>(112),

	//solver_variant::S<nx::EP2, nx::EO4,   8>(204),
	solver_variant::S<nx::EP2, nx::EO8,   9>(208),
	solver_variant::S<nx::EP2, nx::EO12, 10>(212),

	solver_variant::S<nx::EP3, nx::EO4,   8>(304),
	solver_variant::S<nx::EP3, nx::EO8,  10>(308),
	solver_variant::S<nx::EP3, nx::EO12, 10>(312), // base 11 reduces lookups by 0.2%

	solver_variant::S<nx::EP4, nx::EO4,  10>(404),
	//solver_variant::S<nx::EP4, nx::EO8,  ??>(408),
	//solver_variant::S<nx::EP4, nx::EO12, ??>(412),
};
static constexpr int DEFAULT_VARIANT = 308;

int main(int argc, char * const *argv) {
	cf.path = base_path(argv[0]);
	cf.workers = std::max(1U, std::thread::hardware_concurrency());
	cf.coord = DEFAULT_VARIANT;
	cf.style = moveseq_t::SINGMASTER;
	cf.format = FMT_MOVES;
	cf.speffz_buffer = { 'A', 'U' };
	cf.no_input = false;
	cf.inverse = false;

	for (;;) {
		static struct option long_options[] = {
			{ "coord",    required_argument, 0, 'c' },
			{ "format",   required_argument, 0, 'f' },
			{ "help",     no_argument,       0, 'h' },
			{ "inverse",  no_argument,       0, 'i' },
			{ "no-input", no_argument,       0, 'n' },
			{ "shm",      no_argument,       0, 'S' },
			{ "speffz",   optional_argument, 0, 'z' },
			{ "style",    required_argument, 0, 's' },
			{ "workers",  required_argument, 0, 'w' },
			{ NULL }
		};

		int option_index = 0;
		int this_option_optind = optind ? optind : 1;
		int c = getopt_long(argc, argv, "c:f:hinSs:w:z::", long_options, &option_index);
		if (c == -1) {
			break;
		}

		int len;
		switch (c) {
		    case 'c':
			cf.coord = strtoul(optarg, NULL, 10);
			break;
		    case 'f':
			len = strlen(optarg);
			if (!strncmp(optarg, "moves", len)) {
				cf.format = FMT_MOVES;
			} else if (!strncmp(optarg, "reid", len)) {
				cf.format = FMT_REID;
			} else if (!strncmp(optarg, "speffz", len)) {
				cf.format = FMT_SPEFFZ;
			} else {
				fprintf(stderr, "Unsupported input format '%s'\n", optarg);
				usage(argv[0]);
			}
			break;
		    case 'z':
			cf.format = FMT_SPEFFZ;
			if (optarg) {
				if (optarg[0]) {
					cf.speffz_buffer[0] = optarg[0]; // corner
					if (optarg[1]) {
						cf.speffz_buffer[1] = optarg[1]; // edge
					}
				}
			}
			break;
		    case 'n':
			cf.no_input = true;
			break;
		    case 'S':
			cf.shm = true;
			break;
		    case 's':
			len = strlen(optarg);
			if (!strncmp(optarg, "human", len)) {
				cf.style = moveseq_t::SINGMASTER;
			} else if (!strncmp(optarg, "fixed", len)) {
				cf.style = moveseq_t::FIXED;
			} else {
				fprintf(stderr, "Unsupported output style '%s'\n", optarg);
				usage(argv[0]);
			}
			break;
		    case 'i':
			cf.inverse = true;
			break;
		    case 'w':
			cf.workers = strtoul(optarg, NULL, 10);
			break;
		    default:
			usage(argv[0], EXIT_SUCCESS);
		}
	}

	setbuf(stdout, NULL);

	for (auto &S : solvers) {
		if (S.id == cf.coord) {
			S();
			return 0;
		}
	}

	fprintf(stderr, "Unsupported edge coordinate '%d'\n", cf.coord);
	exit(EXIT_FAILURE);
}

std::string format_table_size(size_t n) {
	const char suffix[] = " kMGTPE";
	static char s[64];
	int e = (63 - _lzcnt_u64(n)) / 10;
	sprintf(s, "%.3f", double(n) / (1LL << (10 * e)));
	sprintf(s + 5, " %c%cB", " kMGTPE"[e], "i "[!e]);
	return s;
}

void usage(const char *argv0, int status) {
	fprintf(stdout, "Usage: %s [OPTION]...\n", argv0);
	fputs(	 /**********************************************************************/
		"Optimal half-turn metric Rubik's cube solver.\n"
		"\n"
		"Input cubes are read from standard input, one per line.\n"
		"Solutions are output in the order they are found, which may differ from\n"
		"the input order.  Each output line includes the input sequence number.\n"
		"Example output:\n"
		"  7 68.926868516 20 U3L3U2F1D1R3L2B1L3U3L2U3F2D3F2R1U3L2F1B1\n"
		"The fields are:\n"
		"  Sequence number, time to solve, solution length, solution\n"
		"\n"
		"Options:\n"
		"  -h, --help\n"
		"  -c, --coord=COORD           pruning coordinate variant\n"
		"  -f, --format=FORMAT         input format\n"
		"  -z, --speffz=[C[E]]         speffz buffers (implies -f speffz)\n"
		"  -n, --no-input              load/generate tables and exit\n"
		"  -S, --shm                   load table into shared memory\n"
		"  -s, --style=STYLE           output style\n"
		"  -i, --inverse               output scrambles instead of solutions\n"
		"  -w, --workers=NUM           worker count (default: cpu core count)\n"
		"\n"
		"Pruning coordinate variants (COORD):\n"
		, stdout);
	std::sort(solvers.begin(), solvers.end());
	for (auto &S : solvers) {
		fprintf(stdout, "  %3d (%s)%s\n",
				S.id, format_table_size(S.size).c_str(),
				S.id == DEFAULT_VARIANT ? " [default]" : "");
	}
	fputs(	 /**********************************************************************/
		"\n"
		"Input formats (FORMAT):\n"
		"  moves [default]\n"
		"  reid\n"
		"  speffz (buffers: corner=A, edge=U)\n"
		"\n"
		"Output styles (STYLE):\n"
		"  human (U' R  F2) [default]\n"
		"  fixed (U3R1R2)\n"
		"\n"
		"The input format used by Michael Reid's solver has the following identity:\n"
		"  UF UR UB UL DF DR DB DL FR FL BR BL UFR URB UBL ULF DRF DFL DLB DBR\n"
		"\n"
		"Speffz is a lettering scheme used in blindfolded solving.  The 24\n"
		"corner and 24 edge stickers are assigned letters A through X.\n"
		"This input format looks like \"corneRs.edGes\", and describes a\n"
		"sequence of swaps and in-place reorientations that will solve the cube.\n"
		"Lowercase letters specify a sticker that will be swapped with the\n"
		"buffer.  Uppercase edges are flipped in place; uppercase corner\n"
		"stickers are twisted toward the up/down face.  In place reorientations\n"
		"also affect the buffer in the opposite direction.\n"
		"\n"
		"Example speffz input notation (A/U buffers) for nested cubes pattern:\n"
		"  olpibpMH.etlaol == U' R D' F' R U2 R2 U' R' U R2 L D' L' F2 D2 R'\n"
		, stdout);
	exit(status);
}

std::string base_path(const char *argv0) {
	char *tmp = realpath(argv0, NULL);
	if (!tmp) {
	 	perror("realpath");
		exit(EXIT_FAILURE);
	}
	std::string path(dirname(tmp));
	free(tmp);
	return path;
}

class cpu_clock {
    public:
	using duration = std::chrono::microseconds;
	using time_point = std::chrono::time_point<cpu_clock>;;
	static time_point now() noexcept {
		struct rusage ru;
		getrusage(RUSAGE_SELF, &ru);
		return time_point(duration(
			(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
			(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec)));
	}
};

template<nx::EPvariant EP, nx::EOvariant EO, int Base>
void solver(const std::string &table_filename, uint32_t shm_key) {
	using ECoord = nx::ecoord<EP, EO>;
	using Prune = nx::prune<ECoord, Base>;

	Prune P;

	std::string table_fullpath = cf.path + "/" + table_filename;
	if (!P.loadShared(shm_key)) {
		bool ok;
		if (cf.shm) {
			ok = P.loadShared(shm_key, table_fullpath);
		} else {
			ok = P.load(table_fullpath);
		}
		if (!ok) {
			nx::prune_generator gen(P, cf.workers);
			gen.generate();
			P.save(table_fullpath);
		}
	}

	if (cf.no_input) {
		// generate tables only
		return;
	}

	nx::solver_base::init();

	auto t0 = std::chrono::steady_clock::now();
	auto cpu_t0 = cpu_clock::now();

	uint64_t next_id = 1;

	std::mutex mtx;
	std::vector<std::thread> workers;
	uint64_t best_size_htm = 20;
	uint64_t best_size_qtm = 26;
	for (int i = 0; i < cf.workers; i++) {
		workers.push_back(std::thread([&mtx, &P, &next_id, &best_size_htm, &best_size_qtm]() {
					char buf[1024];
					nx::solver S(P);
					mtx.lock();
					while (!feof(stdin) && fgets(buf, sizeof(buf), stdin)) {
						uint64_t solution_id = next_id++;
						int synced_best_size = best_size_htm;
						mtx.unlock();

						cube c;
						switch (cf.format) {
						    case FMT_MOVES:
							c = cube::from_moves(buf);
							break;
						    case FMT_REID:
							c = cube::from_reid(buf);
							break;
						    case FMT_SPEFFZ:
							c = cube::from_speffz(buf, cf.speffz_buffer[0], cf.speffz_buffer[1]);
							break;
						}
						if (cf.inverse) c = ~c;

						auto moves = S.solve(c, synced_best_size);

						moves = moves.canonical();
						uint64_t size_htm = moves.size();
						uint64_t size_qtm = moves.size_qtm();
						mtx.lock();
						if (size_htm > 0 && (
							size_htm < best_size_htm ||
							size_htm == best_size_htm && size_qtm <= best_size_qtm
						)) {
							best_size_htm = size_htm;
							best_size_qtm = size_qtm;
							std::string solution = moves.to_string(cf.style);
							snprintf(buf, sizeof(buf), "%lu: %luhtm %luqtm %s",
									solution_id,
									best_size_htm,
									best_size_qtm,
									solution.c_str());
							puts(buf);
						}
					}
					mtx.unlock();
					}));
	}

	for (auto &t : workers) {
		t.join();
	}

	std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - t0;
	std::chrono::duration<double> cpu_elapsed = cpu_clock::now() - cpu_t0;
	fprintf(stderr, "Total time: %.9f real, %.6f cpu, %.6f cpu/worker\n",
			elapsed.count(),
			cpu_elapsed.count(),
			cpu_elapsed.count() / cf.workers);
}
