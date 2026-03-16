
#include <stdio.h>
#include <iostream>
#include <cmath>
#include <vector>
#include <string>
#include <memory>
#include <fstream>
#include <filesystem>
#include <map>
#include <algorithm>

#include "portable-file-dialogs.h"

#include <mach-o/dyld.h>
#include <libgen.h>
#include <unistd.h>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#ifndef GL_R32F
#define GL_R32F 0x822E
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif

void SetupMacOSBundlePath()
{
	char path[1024];
	uint32_t size = sizeof(path);
	if (_NSGetExecutablePath(path, &size) == 0)
	{
		// exec_dir は .../FitsViewer.app/Contents/MacOS
		char* exec_dir = dirname(path);
		// Resources ディレクトリへのパス
		std::string res_path = std::string(exec_dir) + "/../Resources";
		chdir(res_path.c_str());
	}
}

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

inline uint32_t swap32(uint32_t val)
{
	return ((val << 24) & 0xff000000) | ((val << 8) & 0x00ff0000) | ((val >> 8) & 0x0000ff00) | ((val >> 24) & 0x000000ff);
}

struct TableColumn
{
	std::string name;
	std::vector<float> data;
};

struct HDUExtension
{
	std::string name;
	std::vector<TableColumn> columns;
};

struct FitsData
{
	int naxis1 = 0;
	int naxis2 = 0;
	int naxis3 = 1;
	int bitpix = 0;
	std::vector<std::string> header_cards;
	std::vector<float> cube; // データ本体

	std::vector<HDUExtension> extensions;

	float wcs_crval = 0.0f;
	float wcs_cdelt = 1.0f;
	float wcs_crpix = 1.0f;
};

std::string trim_fits_val_internal(const std::string& card)
{
	if (card.length() < 10)
	{
		return "";
	}

	std::string v = card.substr(10);
	size_t slash = v.find('/');

	if (slash != std::string::npos)
	{
		v = v.substr(0, slash);
	}
	
	size_t f = v.find_first_not_of(" ");
	size_t l = v.find_last_not_of(" ");

	if (f == std::string::npos)
	{
		return "";
	}

	v = v.substr(f, l - f + 1);

	if (!v.empty() && v.front() == '\'')
	{
		size_t second_q = v.find('\'', 1);

		if (second_q != std::string::npos)
		{
			v = v.substr(1, second_q - 1);
			size_t last = v.find_last_not_of(" ");
			v = (last == std::string::npos) ? "" : v.substr(0, last + 1);
		}
	}

	return v;
}

bool readFITS3D(const std::string& path, FitsData& out)
{
	{
		std::ifstream ifs(path, std::ios::binary);
		if (!ifs) return false;

		char block[2881];
		block[2880] = '\0';
		bool end_found = false;

		// Header
		while (ifs.read(block, 2880) && !end_found)
		{
			for (int i = 0; i < 36; ++i)
			{
				std::string card(block + i * 80, 80);
				out.header_cards.push_back(card);

				std::string key = card.substr(0, 8);
				key.erase(key.find_last_not_of(' ') + 1);

				if (key == "END") { end_found = true; break; }

				auto get_val = [&card]()
				{
					std::string v = card.substr(10, 20);
					size_t first = v.find_first_not_of(" '");
					size_t last = v.find_last_not_of(" '");
					return (first == std::string::npos) ? "" : v.substr(first, last - first + 1);
				};

				std::string val = get_val();
				if (val.empty()) continue;

				if (key == "BITPIX") out.bitpix = std::stoi(val);
				if (key == "NAXIS1") out.naxis1 = std::stoi(val);
				if (key == "NAXIS2") out.naxis2 = std::stoi(val);
				if (key == "NAXIS3") out.naxis3 = std::stoi(val);
			}
		}

		size_t total_pixels = static_cast<size_t>(out.naxis1) * out.naxis2 * out.naxis3;
		out.cube.resize(total_pixels);

		if (out.bitpix == -32)
		{
			for (size_t i = 0; i < total_pixels; ++i)
			{
				uint32_t buf;
				ifs.read(reinterpret_cast<char*>(&buf), 4);
				uint32_t swapped = swap32(buf);
				out.cube[i] = *reinterpret_cast<float*>(&swapped);
			}
		} 
		else if (out.bitpix == 16)
		{
			for (size_t i = 0; i < total_pixels; ++i)
			{
				int16_t buf;
				ifs.read(reinterpret_cast<char*>(&buf), 2);

				out.cube[i] = static_cast<float>(__builtin_bswap16(buf)); 
			}
		}
	}

	{
		std::ifstream ifs(path, std::ios::binary);
		if (!ifs) return false;

		char block[2880];
		out.extensions.clear();
		int hdu_idx = 0;

		while (ifs.read(block, 2880))
		{
			std::string extname = "(none)";
			std::string xtype = "PRIMARY";
			int bpix = 8;
			long long naxis = 0;
			std::vector<long long> naxes;
			std::vector<std::string> current_header;
			bool end_found = false;

			while (!end_found)
			{
				for (int i = 0; i < 36; ++i)
				{
					std::string card(block + i * 80, 80);
					current_header.push_back(card);
					std::string key = card.substr(0, 8);
					key.erase(key.find_last_not_of(' ') + 1);
					if (key == "END")
					{
						end_found = true; break;
					}

					std::string val = trim_fits_val_internal(card);
					if (key == "XTENSION") xtype = val;
					if (key == "EXTNAME")  extname = val;
					if (key == "BITPIX")   bpix = std::stoi(val.empty() ? "0" : val);
					if (key == "NAXIS")    naxis = std::stoll(val.empty() ? "0" : val);

					if (key.substr(0, 5) == "NAXIS" && key.length() > 5)
					{
						try
						{
							int idx = std::stoi(key.substr(5));

							if (idx > 0)
							{
								if ((int)naxes.size() < idx)
								{
									naxes.resize(idx);
								}

								naxes[idx - 1] = std::stoll(val);
							}
						}
						catch (...)
						{
							;
						}
					}
				}

				if (!end_found && !ifs.read(block, 2880))
				{
					break;
				}
			}

			if (hdu_idx > 0)
			{
				if (xtype == "TABLE")
				{
					HDUExtension ext;
					ext.name = extname;
					int row_len = (int)naxes[0], num_rows = (int)naxes[1];
					struct ColMeta { std::string name; int start = 0; };
					std::map<int, ColMeta> col_map;

					for (const auto& card : current_header)
					{
						if (card.substr(0, 5) == "TTYPE" || card.substr(0, 5) == "TBCOL")
						{
							int n = std::stoi(card.substr(5, 3));
							if (card.substr(0, 5) == "TTYPE") col_map[n].name = trim_fits_val_internal(card);
							else col_map[n].start = std::stoi(trim_fits_val_internal(card));
						}
					}

					for (auto const& [n, m] : col_map)
					{
						ext.columns.push_back({m.name, std::vector<float>(num_rows)});
					}

					std::vector<char> row_buf(row_len);

					for (int r = 0; r < num_rows; ++r)
					{
						ifs.read(row_buf.data(), row_len);
						int ci = 0;

						for (auto const& [n, m] : col_map)
						{
							int s = m.start - 1;

							if (s >= 0 && s + 12 <= row_len)
							{
								try { ext.columns[ci].data[r] = std::stof(std::string(row_buf.data() + s, 12)); } catch (...) {}
							}

							ci++;
						}
					}
					
					out.extensions.push_back(std::move(ext));
					size_t t_bytes = (size_t)row_len * num_rows;
					ifs.seekg((2880 - (t_bytes % 2880)) % 2880, std::ios::cur);
				}
				else
				{
					HDUExtension ext; ext.name = extname;
					out.extensions.push_back(ext);
					long long total_p = 1;

					for (long long n : naxes)
					{
						total_p *= n;
					}

					size_t byte_s = static_cast<size_t>(total_p) * (std::abs(bpix) / 8);
					ifs.seekg(byte_s + (2880 - (byte_s % 2880)) % 2880, std::ios::cur);
				}
			}
			else
			{
				long long total_p = 1;

				for (long long n : naxes)
				{
					total_p *= n;
				}

				size_t byte_s = static_cast<size_t>(total_p) * (std::abs(bpix) / 8);
				ifs.seekg(byte_s + (2880 - (byte_s % 2880)) % 2880, std::ios::cur);
			}

			hdu_idx++;
		}
	}

	return true;
}

void UpdateWCSParams(FitsData& data, int axis)
{
	std::string s_val  = "CRVAL" + std::to_string(axis);
	std::string s_delt = "CDELT" + std::to_string(axis);
	std::string s_pix  = "CRPIX" + std::to_string(axis);

	data.wcs_crval = 0.0f;
	data.wcs_cdelt = 1.0f;
	data.wcs_crpix = 1.0f;

	for (const auto& card : data.header_cards)
	{
		if (card.length() < 8) continue;
		
		std::string key = card.substr(0, 8);
		key.erase(key.find_last_not_of(' ') + 1);

		auto get_float = [&]()
		{
			try
			{
				std::string v = card.substr(10, 20);
				return std::stof(v);
			}
			catch (...)
			{
				return 0.0f;
			}
		};

		if(key == s_val)
		{
			data.wcs_crval = get_float();
		}
		else if (key == s_delt)
		{
			data.wcs_cdelt = get_float();
		}
		else if (key == s_pix)
		{
			data.wcs_crpix = get_float();
		}
	}
}

struct ColorPoint
{
	float pos;
	float r, g, b;
};

class ColorMapManager
{
	public:
		struct Palette
		{
			std::string name;
			GLuint texture_id;
			std::vector<uint8_t> cpu_data;
		};
		
		std::vector<Palette> palettes;
		int current_palette_idx = 0;

		void add_palette(const std::string& name, const std::vector<ColorPoint>& points)
		{
			std::vector<uint8_t> data(256 * 3);

			for (int i = 0; i < 256; ++i)
			{
				float t = i / 255.0f;
				ColorPoint p1 = points[0], p2 = points.back();

				for (size_t j = 0; j < points.size() - 1; ++j)
				{
					if (t >= points[j].pos && t <= points[j + 1].pos)
					{
						p1 = points[j]; p2 = points[j + 1];
						break;
					}
				}

				float ratio = (p1.pos == p2.pos) ? 0.0f : (t - p1.pos) / (p2.pos - p1.pos);
				data[i * 3 + 0] = (uint8_t)((p1.r + (p2.r - p1.r) * ratio) * 255.0f);
				data[i * 3 + 1] = (uint8_t)((p1.g + (p2.g - p1.g) * ratio) * 255.0f);
				data[i * 3 + 2] = (uint8_t)((p1.b + (p2.b - p1.b) * ratio) * 255.0f);
			}

			GLuint tex;
			glGenTextures(1, &tex);
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, data.data());
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			palettes.push_back({name, tex, data});
		}
};

float CalculateNiceStep(float range, int target_ticks)
{
	if (range == 0)
	{
		return 1.0f;
	}
	
	float rough_step = range / (target_ticks - 1);
	float exponent = std::floor(std::log10(rough_step));
	float fraction = rough_step / std::pow(10.0f, exponent);
	
	float nice_fraction;
	if (fraction < 1.5f)
	{
		nice_fraction = 1.0f;
	}
	else if (fraction < 3.0f)
	{
		nice_fraction = 2.0f;
	}
	else if (fraction < 7.0f)
	{
		nice_fraction = 5.0f;
	}
	else
	{
		nice_fraction = 10.0f;
	}
	
	return nice_fraction * std::pow(10.0f, exponent);
}

void DrawCustomSpectrum(const std::vector<float>& data, const std::vector<float>& x_grid, float x_min, float x_max, float y_min, float y_max, int current_slice)
{
	float x_range = x_max - x_min;
	float x_step = CalculateNiceStep(x_range, 8);
	float y_range = y_max - y_min;
	float y_step = CalculateNiceStep(y_range, 5);
	
	if (x_range <= 0.0f)
	{
		x_range = 1.0f;
	}

	if (y_range <= 0.0f)
	{
		y_range = 1.0f;
	}

	if (x_step <= 0.0f)
	{
		x_step = x_range;
	}

	if (y_step <= 0.0f)
	{
		y_step = y_range;
	}

	ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
	ImVec2 canvas_sz = ImVec2(ImGui::GetContentRegionAvail().x, 220);

	if (canvas_sz.x < 100.0f)
	{
		canvas_sz.x = 100.0f;
	}

	ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

	const float m_left = 80.0f, m_right = 20.0f, m_top = 10.0f, m_bottom = 30.0f;
	ImVec2 plot_p0 = ImVec2(canvas_p0.x + m_left, canvas_p0.y + m_top);
	ImVec2 plot_p1 = ImVec2(canvas_p1.x - m_right, canvas_p1.y - m_bottom);
	ImVec2 plot_sz = ImVec2(plot_p1.x - plot_p0.x, plot_p1.y - plot_p0.y);

	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	
	draw_list->AddRectFilled(plot_p0, plot_p1, IM_COL32(15, 15, 15, 255));

	if (data.empty() || plot_sz.x <= 0 || plot_sz.y <= 0)
	{
		ImGui::Dummy(canvas_sz);
		return;
	}

	auto ToCanvas = [&](float wx, float wy) -> ImVec2
	{
		float x_norm = (x_max != x_min) ? (wx - x_min) / (x_max - x_min) : 0.0f;
		float y_norm = (y_max != y_min) ? (wy - y_min) / (y_max - y_min) : 0.5f;
		return ImVec2(plot_p0.x + x_norm * plot_sz.x, plot_p1.y - y_norm * plot_sz.y);
	};

	draw_list->PushClipRect(plot_p0, plot_p1, true);

	ImU32 tick_col = IM_COL32(160, 160, 160, 255);
	ImU32 grid_col = IM_COL32(45, 45, 45, 255);
	char buf[32];

	float start_x = std::floor(x_min / x_step) * x_step;

	for (float val = start_x; val <= x_max + (x_step * 0.01f); val += x_step)
	{
		ImVec2 p = ToCanvas(val, y_min);
		draw_list->AddLine(ImVec2(p.x, plot_p0.y), ImVec2(p.x, plot_p1.y), grid_col);
		draw_list->AddLine(ImVec2(p.x, plot_p1.y), ImVec2(p.x, plot_p1.y + 5), tick_col);
	}

	float start_y = std::floor(y_min / y_step) * y_step;

	for (float val = start_y; val <= y_max + (y_step * 0.01f); val += y_step)
	{
		ImVec2 p = ToCanvas(x_min, val); 
		draw_list->AddLine(ImVec2(plot_p0.x, p.y), ImVec2(plot_p1.x, p.y), grid_col);
		draw_list->AddLine(ImVec2(plot_p0.x - 5, p.y), ImVec2(plot_p0.x, p.y), tick_col);
		
		snprintf(buf, 32, "%.2e", val);
		ImVec2 text_size = ImGui::CalcTextSize(buf);
		draw_list->AddText(ImVec2(plot_p0.x - 10.0f - text_size.x, p.y - text_size.y * 0.5f), tick_col, buf);
	}

	if (!x_grid.empty())
	{
		auto it_s = std::lower_bound(x_grid.begin(), x_grid.end(), x_min);
		auto it_e = std::lower_bound(x_grid.begin(), x_grid.end(), x_max);
		int start_i = (int)std::max(0, (int)std::distance(x_grid.begin(), it_s) - 1);
		int end_i   = (int)std::min((int)data.size() - 1, (int)std::distance(x_grid.begin(), it_e) + 1);

		const ImU32 order_colors[] =
		{
			IM_COL32(0, 255, 255, 255),
			IM_COL32(255, 100, 255, 255),
			IM_COL32(255, 255, 100, 255),
			IM_COL32(100, 255, 100, 255),
			IM_COL32(255, 150, 50, 255),
			IM_COL32(150, 150, 255, 255)
		};

		// const ImU32 order_colors[] =
		// {
		//     IM_COL32(31,  119, 180, 255),
		//     IM_COL32(255, 127, 14,  255),
		//     IM_COL32(44,  160, 44,  255),
		//     IM_COL32(214, 39,  40,  255),
		//     IM_COL32(148, 103, 189, 255),
		//     IM_COL32(140, 86,  75,  255),
		//     IM_COL32(227, 119, 194, 255),
		//     IM_COL32(127, 127, 127, 255),
		//     IM_COL32(188, 189, 34,  255),
		//     IM_COL32(23,  190, 207, 255)
		// };

		const int num_colors = sizeof(order_colors) / sizeof(ImU32);

		int current_order_idx = 0;

		for (int i = 0; i < start_i; i++)
		{
			if (i + 1 < (int)x_grid.size() && x_grid[i+1] <= x_grid[i])
			{
				current_order_idx++;
			}
		}

		for (int i = start_i; i < end_i; i++)
		{
			if (x_grid[i+1] <= x_grid[i])
			{
				current_order_idx++;
				continue;
			}

			ImU32 col = order_colors[current_order_idx % num_colors];
			
			draw_list->AddLine(ToCanvas(x_grid[i], data[i]), ToCanvas(x_grid[i+1], data[i+1]), col, 1.5f);
		}
	}

	if (current_slice >= 0 && current_slice < (int)x_grid.size()) 
	{
		float phys_x = x_grid[current_slice];

		float x_low = std::min(x_min, x_max);
		float x_high = std::max(x_min, x_max);

		if (phys_x >= x_low && phys_x <= x_high) 
		{
			float line_x = ToCanvas(phys_x, y_min).x;
			
			draw_list->AddLine(ImVec2(line_x, plot_p0.y), ImVec2(line_x, plot_p1.y), IM_COL32(255, 0, 0, 160), 3.0f);
		}
	}

	draw_list->PopClipRect();

	for (float val = start_x; val <= x_max + (x_step * 0.01f); val += x_step)
	{
		if (val < x_min || val > x_max)
		{
			continue;
		}

		float px = ToCanvas(val, y_min).x;
		snprintf(buf, 32, "%g", val);
		ImVec2 ts = ImGui::CalcTextSize(buf);

		draw_list->AddText(ImVec2(px - ts.x * 0.5f, plot_p1.y + 7), tick_col, buf);
	}

	for (float val = start_y; val <= y_max + (y_step * 0.01f); val += y_step)
	{
		if (val < y_min || val > y_max)
		{
			continue;
		}

		float py = ToCanvas(0, val).y;

		draw_list->AddLine(ImVec2(plot_p0.x - 5, py), ImVec2(plot_p0.x, py), tick_col);

		snprintf(buf, 32, "%.2e", val);
		ImVec2 ts = ImGui::CalcTextSize(buf);

		draw_list->AddText(ImVec2(plot_p0.x - 10.0f - ts.x, py - ts.y * 0.5f), tick_col, buf);
	}

	draw_list->AddRect(plot_p0, plot_p1, IM_COL32(100, 100, 100, 255));
	ImGui::Dummy(canvas_sz);
}

std::string global_dropped_path = "";

void drop_callback(GLFWwindow* window, int count, const char** paths)
{
	if (count > 0)
	{
		global_dropped_path = paths[0];
	}
}

struct ROI
{
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;

	bool active = false;
	bool visible = false;
};

struct ViewState
{
	float x_min = 0;
	float x_max = 0;
	float y_min = 0;
	float y_max = 0;
	float offset_x = 0;
	float offset_y = 0;
	float scale = 1.0f;

	bool initialized = false;
};

enum class XAxisMode
{
	Index,
	WCS,
	Table
};

struct SpecViewState
{
	float x_min = 0.0f;
	float x_max = 1.0f;
	float y_min = 0.0f;
	float y_max = 1.0f;

	bool initialized = false;

	XAxisMode mode = XAxisMode::Index;
};

enum class ToolMode
{ 
	None,
	Zoom,
	Average
};

std::vector<float> ExtractPointSpectrum(const FitsData& data, int ix, int iy, const int axis_map[3], const int n_dims[3])
{
	std::vector<float> spec;
	int nz = n_dims[axis_map[2]]; 
	spec.reserve(nz);

	if (data.cube.empty()) return spec;

	for (int i = 0; i < nz; ++i)
	{
		int coords[3] = {0, 0, 0};
		coords[axis_map[0]] = ix;
		coords[axis_map[1]] = iy;
		coords[axis_map[2]] = i; 

		int rx = std::clamp(coords[0], 0, (int)data.naxis1 - 1);
		int ry = std::clamp(coords[1], 0, (int)data.naxis2 - 1);
		int rz = std::clamp(coords[2], 0, (int)data.naxis3 - 1);

		size_t idx = (size_t)rz * ((size_t)data.naxis1 * data.naxis2) + (size_t)ry * data.naxis1 + (size_t)rx;
		
		if (idx < data.cube.size())
		{
			float val = data.cube[idx];
			
			if (std::isfinite(val) && val > -1.0e37f && val < 1.0e37f) 
			{
				spec.push_back(val);
			}
			else 
			{
				spec.push_back(0.0f);
			}
		}
		else
		{
			spec.push_back(0.0f);
		}
	}

	return spec;
}

std::vector<float> CalculateAverageSpectrum(const FitsData& data, const ROI& roi, const int axis_map[3], const int n_dims[3])
{
	int nz = n_dims[axis_map[2]];
	std::vector<float> avg_spec(nz, 0.0f);
	std::vector<int> valid_counts(nz, 0);

	int x_start = std::clamp(std::min(roi.x0, roi.x1), 0, n_dims[axis_map[0]] - 1);
	int x_end   = std::clamp(std::max(roi.x0, roi.x1), 0, n_dims[axis_map[0]] - 1);
	int y_start = std::clamp(std::min(roi.y0, roi.y1), 0, n_dims[axis_map[1]] - 1);
	int y_end   = std::clamp(std::max(roi.y0, roi.y1), 0, n_dims[axis_map[1]] - 1);

	if (x_start > x_end || y_start > y_end)
	{
		return avg_spec;
	}

	for (int iy = y_start; iy <= y_end; ++iy)
	{
		for (int ix = x_start; ix <= x_end; ++ix)
		{
			for (int iz = 0; iz < nz; ++iz)
			{
				int c[3];
				c[axis_map[0]] = ix;
				c[axis_map[1]] = iy;
				c[axis_map[2]] = iz;

				int rx = std::clamp(c[0], 0, (int)data.naxis1 - 1);
				int ry = std::clamp(c[1], 0, (int)data.naxis2 - 1);
				int rz = std::clamp(c[2], 0, (int)data.naxis3 - 1);

				size_t idx = (size_t)rz * ((size_t)data.naxis1 * data.naxis2) + (size_t)ry * data.naxis1 + (size_t)rx;

				if (idx < data.cube.size())
				{
					float val = data.cube[idx];
					if (std::isfinite(val))
					{
						avg_spec[iz] += val;
						valid_counts[iz]++;
					}
				}
			}
		}
	}

	for (int i = 0; i < nz; ++i)
	{
		if (valid_counts[i] > 0)
		{
			avg_spec[i] /= (float)valid_counts[i];
		}
		else
		{
			avg_spec[i] = NAN;
		}
	}

	return avg_spec;
}

void ScreenToFitsIdx(ImVec2 m, int* ix, int* iy, const ViewState& vp, ImVec2 p0, ImVec2 sz, int fits_w, int fits_h)
{
	float u = std::clamp((m.x - p0.x) / sz.x, 0.0f, 1.0f);
	float v = std::clamp((m.y - p0.y) / sz.y, 0.0f, 1.0f);
	
	*ix = (int)(vp.x_min + u * (vp.x_max - vp.x_min));
	*iy = (int)(vp.y_min + (1.0f - v) * (vp.y_max - vp.y_min));

	*ix = std::clamp(*ix, 0, fits_w - 1);
	*iy = std::clamp(*iy, 0, fits_h - 1);
}

void ApplyZoom(const ROI& selected_roi, ViewState& vp)
{
	if (std::abs(selected_roi.x1 - selected_roi.x0) < 2 || std::abs(selected_roi.y1 - selected_roi.y0) < 2)
	{
		return;
	}

	vp.x_min = (float)std::min(selected_roi.x0, selected_roi.x1);
	vp.x_max = (float)std::max(selected_roi.x0, selected_roi.x1);
	vp.y_min = (float)std::min(selected_roi.y0, selected_roi.y1);
	vp.y_max = (float)std::max(selected_roi.y0, selected_roi.y1);
}

void ResetView(ViewState& vp, int fits_w, int fits_h)
{
	vp.x_min = 0.0f;
	vp.y_min = 0.0f;
	vp.x_max = (float)fits_w;
	vp.y_max = (float)fits_h;

	vp.offset_x = 0.0f; 
	vp.offset_y = 0.0f;
	vp.scale = 1.0f;
	
	vp.initialized = true;
}

void ResetSpecView(SpecViewState& vp, const std::vector<float>& spec)
{
	if (spec.empty())
	{
		vp.initialized = false;
		return;
	}

	auto [s_min_it, s_max_it] = std::minmax_element(spec.begin(), spec.end());

	vp.x_min = 0.0f;
	vp.x_max = (float)spec.size();
	vp.y_min = *s_min_it;
	vp.y_max = *s_max_it;

	float padding = (vp.y_max - vp.y_min) * 0.1f;

	if (padding == 0.0f)
	{
		padding = 1.0f;
	}

	vp.y_min -= padding;
	vp.y_max += padding;

	vp.initialized = true;
}

void ResetSpecView(SpecViewState& vp, const std::vector<float>& spectrum, const std::vector<float>& x_grid)
{
	if (!x_grid.empty())
	{
		vp.x_min = x_grid.front();
		vp.x_max = x_grid.back();
	}
	else
	{
		vp.x_min = 0.0f;
		vp.x_max = spectrum.empty() ? 1.0f : (float)spectrum.size() - 1;
	}

	vp.initialized = false;
}

int main(int argc, char** argv)
{
	SetupMacOSBundlePath();

	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit()) return 1;

	const char* glsl_version = "#version 150";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

	GLFWwindow* window = glfwCreateWindow(1700, 1000, "FITS Viewer", NULL, NULL);
	if (window == NULL) return 1;

	const float SIDEBAR_W = 500.0f;
	const float IMAGE_MIN_W = 750.0f;
	const float BOTTOM_H = 320.0f;
	const float CONTROLS_H = 110.0f;
	glfwSetWindowSizeLimits(window, (int)(SIDEBAR_W + IMAGE_MIN_W), 600, GLFW_DONT_CARE, GLFW_DONT_CARE);
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.FontGlobalScale = 1.25f;
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	GLuint image_texture = 0;
	glGenTextures(1, &image_texture);
	glBindTexture(GL_TEXTURE_2D, image_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glfwSetDropCallback(window, drop_callback);

	ColorMapManager cmap_manager;

	cmap_manager.add_palette("Parula",
	{
		{0.0f, 0.2078f, 0.1647f, 0.5294f}, {0.1f, 0.1255f, 0.3255f, 0.8314f},
		{0.2f, 0.0510f, 0.4588f, 0.8627f}, {0.3f, 0.0471f, 0.5765f, 0.8235f},
		{0.4f, 0.0275f, 0.6627f, 0.7608f}, {0.5f, 0.2196f, 0.7255f, 0.6196f},
		{0.6f, 0.4863f, 0.7490f, 0.4824f}, {0.7f, 0.7176f, 0.7412f, 0.2902f},
		{0.8f, 0.9451f, 0.7255f, 0.2902f}, {0.9f, 0.9804f, 0.8275f, 0.1647f},
		{1.0f, 0.9765f, 0.9843f, 0.0549f}
	});

	cmap_manager.add_palette("Jet",
	{
		{0.0f/5.3f, 0.0f, 0.0f, 139/255.0f},
		{1.0f/5.3f, 44/255.0f, 169/255.0f, 225/255.0f},
		{2.0f/5.3f, 56/255.0f, 180/255.0f, 139/255.0f},
		{3.5f/5.3f, 1.0f, 1.0f, 0.0f},
		{5.0f/5.3f, 235/255.0f, 97/255.0f, 1/255.0f},
		{5.3f/5.3f, 201/255.0f, 23/255.0f, 30/255.0f}
	});

	cmap_manager.add_palette("Spectral",
	{
		{0.0000f, 0.2298f, 0.2987f, 0.7537f},
		{0.1250f, 0.3830f, 0.5094f, 0.9174f},
		{0.2500f, 0.5530f, 0.6889f, 0.9954f},
		{0.3750f, 0.7222f, 0.8140f, 0.9766f},
		{0.5000f, 0.8654f, 0.8654f, 0.8654f},
		{0.6250f, 0.9589f, 0.7698f, 0.6780f},
		{0.7500f, 0.9580f, 0.6028f, 0.4818f},
		{0.8750f, 0.8692f, 0.3783f, 0.3003f},
		{1.0000f, 0.7057f, 0.0156f, 0.1502f}
	});

	cmap_manager.add_palette("Grayscale", {{0.0, 0,0,0}, {1.0, 1,1,1}});

	int axis_x = 0;
	int axis_y = 1;

	std::string axis_label_strings[3] = {"NAXIS1", "NAXIS2", "NAXIS3"};
	
	const char* axis_names[3] =
	{ 
		axis_label_strings[0].c_str(), 
		axis_label_strings[1].c_str(), 
		axis_label_strings[2].c_str() 
	};

	int current_slice = 0;
	float v_min = 0.0f;
	float v_max = 1.0f;
	bool needs_update = true;
	int selected_cmap = 0;

	const char* cmap_names[] = {"Parula", "Jet", "Spectral", "Grayscale"};

	int clicked_ix = -1, clicked_iy = -1;

	FitsData data;

	ToolMode current_tool = ToolMode::None;
	ROI drag_roi;
	ROI persistent_roi;
	bool show_persistent_roi = false;
	
	ViewState app_view;
	SpecViewState app_spec_view;
	std::vector<float> spectrum;

	int axis_map[3] = {axis_x, axis_y, 2};
	int n_dims[3] = {data.naxis1, data.naxis2, data.naxis3};

	if (argc > 1)
	{
		std::string initial_path = argv[1];
		global_dropped_path = initial_path;
	}

	std::string current_file_path = "No file loaded";
	std::string current_file_name = "None";
	std::string last_directory = getenv("HOME");

	bool spec_dragging = false;
	float spec_drag_start_x = 0.0f;
	float spec_drag_current_x = 0.0f;

	int drag_button = -1;

	int selected_hdu_idx = -1;
	int selected_col_idx = -1;

	std::vector<float>* active_lut = nullptr;
	std::vector<float> spectral_grid;

	int selected_wcs_axis = 3;
	
	while (!glfwWindowShouldClose(window))
	{
		glfwPollEvents();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		ImGuiIO& io = ImGui::GetIO();
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImVec2 work_pos = viewport->WorkPos;
		ImVec2 work_size = viewport->WorkSize;

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImVec2 v_pos  = vp->WorkPos;
		ImVec2 v_size = vp->WorkSize;

		float main_w = v_size.x - SIDEBAR_W;
		float main_h = v_size.y - BOTTOM_H;

		ImGuiWindowFlags static_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

		ImGui::SetNextWindowPos(v_pos);
		ImGui::SetNextWindowSize(ImVec2(SIDEBAR_W, CONTROLS_H));
		ImGui::Begin("FITS Controller", nullptr, static_flags);
		std::string file_to_load = "";
		
		if (ImGui::Button("Open FITS File"))
		{
			auto selection = pfd::open_file("Select a FITS file", last_directory, { "FITS files", "*.fits *.fit", "All files", "*" }).result();
			if (!selection.empty())
			{
				file_to_load = selection[0];
			}
		}

		ImGui::SameLine();
		ImGui::TextDisabled("or drop FITS");

		if (!global_dropped_path.empty())
		{
			file_to_load = global_dropped_path;
			global_dropped_path = "";
		}

		if (!file_to_load.empty())
		{
			FitsData new_data;
			if (readFITS3D(file_to_load, new_data))
			{
				data = std::move(new_data);

				current_file_path = file_to_load;
				current_file_name = std::filesystem::path(current_file_path).filename().string();
				last_directory = std::filesystem::path(current_file_path).parent_path().string();

				current_slice = 0;
				axis_x = 0;
				axis_y = 1;
				axis_map[0] = 0;
				axis_map[1] = 1;
				axis_map[2] = 2;
				selected_hdu_idx = -1;
				selected_col_idx = -1;

				for (int n = 1; n <= 3; ++n)
				{
					std::string key = "CTYPE" + std::to_string(n);

					for (const auto& card : data.header_cards)
					{
						if (card.substr(0, 8).find(key) != std::string::npos)
						{
							std::string v = card.substr(10, 20);
							size_t first = v.find_first_not_of(" '");
							size_t last = v.find_last_not_of(" '");

							if (first != std::string::npos) 
							{
								axis_label_strings[n-1] = "AXIS" + std::to_string(n) + " (" + v.substr(first, last - first + 1) + ")";
							}

							break;
						}
					}
				}

				for (int i = 0; i < 3; ++i)
				{
					axis_names[i] = axis_label_strings[i].c_str();
				}

				ResetView(app_view, data.naxis1, data.naxis2);
				app_spec_view.initialized = false; 
				app_spec_view.x_min = 0; 
				app_spec_view.x_max = 1;
				app_spec_view.y_min = 0; 
				app_spec_view.y_max = 1;

				clicked_ix = -1;
				clicked_iy = -1;
				show_persistent_roi = false;
				
				spec_dragging = false;
				spectrum.clear();
				spectral_grid.clear();
				active_lut = nullptr;
				app_spec_view.initialized = false;
				app_spec_view.mode = XAxisMode::Index;

				needs_update = true;
			}
			else
			{
				pfd::message("Error", "Failed to load FITS file.", pfd::choice::ok, pfd::icon::error);
			}
		}

		ImGui::Separator();
		ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), "Current File:");
		ImGui::TextWrapped("%s", current_file_name.c_str());
		ImGui::End();

		ImGui::SetNextWindowPos(ImVec2(v_pos.x, v_pos.y + CONTROLS_H));
		ImGui::SetNextWindowSize(ImVec2(SIDEBAR_W, v_size.y - CONTROLS_H));
		ImGui::Begin("FITS Header Viewer", nullptr, static_flags);
		if (data.header_cards.empty())
		{
			ImGui::Text("No data loaded.");
		}
		else
		{
			ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable;

			if (ImGui::BeginTable("HeaderTable", 2, table_flags, ImVec2(0, 0)))
			{
				
				ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 100.0f);
				ImGui::TableSetupColumn("Value / Comment", ImGuiTableColumnFlags_WidthFixed, 800.0f);

				ImGui::TableSetupScrollFreeze(0, 1);

				ImGui::TableHeadersRow();

				for (const auto& card : data.header_cards)
				{
					std::string key = card.substr(0, 8);
					if (key == "END     ") break;

					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(key.c_str());

					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(card.substr(8).c_str());
				}
				ImGui::EndTable();
			}
		}
		ImGui::End();

		ImGui::SetNextWindowPos(ImVec2(v_pos.x + SIDEBAR_W, v_pos.y));
		ImGui::SetNextWindowSize(ImVec2(main_w, main_h));
		ImGui::Begin("FITS Image View", nullptr, static_flags);
		if (!data.cube.empty())
		{
			if (!app_view.initialized)
			{
				ResetView(app_view, data.naxis1, data.naxis2);
				needs_update = true;
			}

			n_dims[0] = data.naxis1;
			n_dims[1] = data.naxis2;
			n_dims[2] = data.naxis3;
			int axis_z = 3 - (axis_x + axis_y);
			
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Min/Max");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(240.0f);

			if (ImGui::DragFloatRange2("##Cut", &v_min, &v_max, (v_max - v_min) * 0.01f))
			{
				needs_update = true;
			}

			ImGui::SameLine();

			if (ImGui::Button("Auto"))
			{
				int nx = n_dims[axis_x];
				int ny = n_dims[axis_y];
				float mi = std::numeric_limits<float>::max();
				float ma = -std::numeric_limits<float>::max();
				bool found = false;

				for (int y = 0; y < ny; ++y)
				{
					for (int x = 0; x < nx; ++x)
					{
						int coords[3];
						coords[axis_x] = x;
						coords[axis_y] = y;
						coords[axis_z] = current_slice;

						size_t idx = (size_t)coords[2] * (data.naxis1 * data.naxis2) + (size_t)coords[1] * data.naxis1 + coords[0];
						
						float val = data.cube[idx];

						if (std::isfinite(val))
						{
							if (val < mi) 
							{
								mi = val;
							}

							if (val > ma) 
							{
								ma = val;
							}

							found = true;
						}
					}
				}
				if (found)
				{
					v_min = mi; v_max = ma;

					if (v_min == v_max)
					{
						v_max = v_min + 1.0f;
					}

					needs_update = true;
				}
			}
			ImGui::SameLine();

			if (ImGui::Button("Reset"))
			{
				v_min = 0.0f; v_max = 1.0f;
				needs_update = true;
			}

			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Color");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(270.0f);

			if (ImGui::Combo("##Map", &selected_cmap, cmap_names, 4))
			{
				needs_update = true;
			}
			
			bool axis_changed = false;

			
			ImGui::AlignTextToFramePadding();
			ImGui::Text("X");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(240.0f);

			if (ImGui::Combo("##X", &axis_x, axis_names, 3))
			{
				axis_changed = true;
			}

			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Y");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(240.0f);

			if (ImGui::Combo("##Y", &axis_y, axis_names, 3))
			{
				axis_changed = true;
			}

			if (axis_x == axis_y)
			{
				axis_y = (axis_x + 1) % 3; axis_changed = true;
			}

			axis_z = 3 - (axis_x + axis_y);
			int z_max = n_dims[axis_z];

			if (axis_changed)
			{
				axis_map[0] = axis_x;
				axis_map[1] = axis_y;
				axis_map[2] = 3 - (axis_x + axis_y);

				ResetView(app_view, n_dims[axis_x], n_dims[axis_y]);

				int z_max = n_dims[axis_map[2]];
				current_slice = std::clamp(current_slice, 0, z_max - 1);

				needs_update = true;
				spectrum.clear();
				show_persistent_roi = false;
				clicked_ix = -1;
				clicked_iy = -1;
				app_spec_view.initialized = false;
				app_spec_view.mode = XAxisMode::Index;
				axis_changed = false;
			}

			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Slice");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);

			if (ImGui::SliderInt("##Slice", &current_slice, 0, z_max - 1))
			{
				needs_update = true;
			}

			ImGui::PopStyleVar();

			ImGui::Separator();
			ImGui::Columns(3, "Guides", false);
			ImGui::SetColumnWidth(0, 250.0f);
			ImGui::BulletText("L Drag: Avr. Spectrum");
			ImGui::BulletText("Mid Drag: Move");
			ImGui::NextColumn();
			ImGui::BulletText("L Click: Pnt. Spectrum");
			ImGui::BulletText("Wheel: Zoom");
			ImGui::NextColumn();
			ImGui::BulletText("R Drag: ROI");
			ImGui::Columns(1);
			ImGui::Separator();
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), "View: ");
			ImGui::SameLine();
			ImGui::Text("X[%.1f : %.1f], Y[%.1f : %.1f]", app_view.x_min, app_view.x_max, app_view.y_min, app_view.y_max);
			ImGui::SameLine();

			if (ImGui::Button("Reset View", ImVec2(100, 0)))
			{
				ResetView(app_view, n_dims[axis_x], n_dims[axis_y]);
				needs_update = true;
			}

			ImVec2 avail_size = ImGui::GetContentRegionAvail();

			const float margin_l = 50.0f, margin_r = 50.0f, margin_t = 30.0f, margin_b = 40.0f;

			float disp_w = std::max(avail_size.x - (margin_l + margin_r), 1.0f);
			float disp_h = std::max(avail_size.y - (margin_t + margin_b), 1.0f);
			float canvas_aspect = disp_h / disp_w;

			float cur_vw = app_view.x_max - app_view.x_min;
			float cur_vh = app_view.y_max - app_view.y_min;

			if (cur_vh / cur_vw < canvas_aspect) 
			{
				float target_vh = cur_vw * canvas_aspect;
				float center_y = (app_view.y_min + app_view.y_max) * 0.5f;
				app_view.y_min = center_y - target_vh * 0.5f;
				app_view.y_max = center_y + target_vh * 0.5f;
			} 
			else 
			{
				float target_vw = cur_vh / canvas_aspect;
				float center_x = (app_view.x_min + app_view.x_max) * 0.5f;
				app_view.x_min = center_x - target_vw * 0.5f;
				app_view.x_max = center_x + target_vw * 0.5f;
			}

			if (needs_update)
			{
				int ix0 = (int)std::floor(app_view.x_min);
				int iy0 = (int)std::floor(app_view.y_min);
				int ix1 = (int)std::ceil(app_view.x_max);
				int iy1 = (int)std::ceil(app_view.y_max);

				int tw = ix1 - ix0;
				int th = iy1 - iy0;

				if (tw >= 1 && th >= 1)
				{
					std::vector<uint8_t> rgb_buf((size_t)tw * th * 3);
					const auto& lut = cmap_manager.palettes[selected_cmap].cpu_data;

					for (int y = 0; y < th; ++y)
					{
						for (int x = 0; x < tw; ++x)
						{
							int tx = ix0 + x;
							int ty = iy0 + y;
							size_t out_idx = (size_t)y * tw + x;

							if (tx >= 0 && tx < n_dims[axis_x] && ty >= 0 && ty < n_dims[axis_y])
							{
								int coords[3];
								coords[axis_x] = tx;
								coords[axis_y] = ty;
								coords[axis_z] = current_slice;

								size_t raw_idx = (size_t)coords[2] * ((size_t)data.naxis1 * data.naxis2) + (size_t)coords[1] * data.naxis1 + coords[0];

								float val = data.cube[raw_idx];
								float norm = std::clamp((val - v_min) / (v_max - v_min), 0.0f, 1.0f);
								int lut_idx = (int)(norm * 255.0f);
								
								rgb_buf[out_idx * 3 + 0] = lut[lut_idx * 3 + 0];
								rgb_buf[out_idx * 3 + 1] = lut[lut_idx * 3 + 1];
								rgb_buf[out_idx * 3 + 2] = lut[lut_idx * 3 + 2];
							}
							else
							{
								rgb_buf[out_idx * 3 + 0] = 30;
								rgb_buf[out_idx * 3 + 1] = 30;
								rgb_buf[out_idx * 3 + 2] = 30;
							}
						}
					}

					glBindTexture(GL_TEXTURE_2D, image_texture);
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_buf.data());
				}

				needs_update = false;
			}	

			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin_l);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + margin_t);

			ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
			ImDrawList* draw_list = ImGui::GetWindowDrawList();

			int ix0 = (int)std::floor(app_view.x_min);
			int iy0 = (int)std::floor(app_view.y_min);
			int ix1 = (int)std::ceil(app_view.x_max);
			int iy1 = (int)std::ceil(app_view.y_max);
			float tw_f = (float)std::max(ix1 - ix0, 1);
			float th_f = (float)std::max(iy1 - iy0, 1);

			float u0 = (app_view.x_min - (float)ix0) / tw_f;
			float u1 = (app_view.x_max - (float)ix0) / tw_f;
			float v0 = (app_view.y_min - (float)iy0) / th_f;
			float v1 = (app_view.y_max - (float)iy0) / th_f;

			ImGui::Image((void*)(intptr_t)image_texture, ImVec2(disp_w, disp_h), ImVec2(u0, v1), ImVec2(u1, v0));

			ImVec2 canvas_p0 = canvas_pos;
			ImVec2 canvas_sz = ImVec2(disp_w, disp_h);

			auto FitsToScreen = [&](float fx, float fy) -> ImVec2
			{
				float rx = (fx - app_view.x_min) / (app_view.x_max - app_view.x_min);
				float ry = (fy - app_view.y_min) / (app_view.y_max - app_view.y_min);
				return ImVec2(canvas_pos.x + rx * disp_w, canvas_pos.y + (1.0f - ry) * disp_h);
			};

			ImU32 col_red  = IM_COL32(255, 60, 60, 255);
			ImU32 col_blue = IM_COL32(60, 150, 255, 255);

			if (show_persistent_roi)
			{
				float x_start = (float)std::min(persistent_roi.x0, persistent_roi.x1);
				float x_end   = (float)std::max(persistent_roi.x0, persistent_roi.x1) + 1.0f;
				float y_start = (float)std::min(persistent_roi.y0, persistent_roi.y1);
				float y_end   = (float)std::max(persistent_roi.y0, persistent_roi.y1) + 1.0f;

				ImVec2 p0 = FitsToScreen(x_start, y_start);
				ImVec2 p1 = FitsToScreen(x_end, y_end);
				
				draw_list->AddRect(p0, p1, col_red, 0.0f, 0, 2.0f);
				draw_list->AddRectFilled(p0, p1, IM_COL32(255, 60, 60, 30)); 
			}
			else if (clicked_ix >= 0 && clicked_iy >= 0)
			{
				ImVec2 p0 = FitsToScreen(clicked_ix, clicked_iy);
				ImVec2 p1 = FitsToScreen(clicked_ix + 1, clicked_iy + 1);
				draw_list->AddRect(p0, p1, col_red, 0.0f, 0, 2.0f);
				draw_list->AddRectFilled(p0, p1, IM_COL32(255, 60, 60, 30));
			}

			if (drag_roi.active)
			{
				float dx0 = (float)std::min(drag_roi.x0, drag_roi.x1);
				float dx1 = (float)std::max(drag_roi.x0, drag_roi.x1) + 1.0f;
				float dy0 = (float)std::min(drag_roi.y0, drag_roi.y1);
				float dy1 = (float)std::max(drag_roi.y0, drag_roi.y1) + 1.0f;

				ImVec2 p0 = FitsToScreen(dx0, dy0);
				ImVec2 p1 = FitsToScreen(dx1, dy1);
				
				ImU32 current_col = (drag_button == 0) ? col_red : col_blue;
				ImU32 fill_col = (drag_button == 0) ? IM_COL32(255, 60, 60, 40) : IM_COL32(60, 150, 255, 40);
				
				draw_list->AddRect(p0, p1, current_col, 0.0f, 0, 2.0f);
				draw_list->AddRectFilled(p0, p1, fill_col);
			}

			ImU32 tick_col = IM_COL32(200, 200, 200, 255);

			float f_xmin = app_view.x_min;
			float f_xmax = app_view.x_max;
			float f_w = f_xmax - f_xmin;

			float f_ymin = app_view.y_min;
			float f_ymax = app_view.y_max;
			float f_h = f_ymax - f_ymin;

			float x_step = CalculateNiceStep(f_w, 7); 
			float start_x = std::floor(f_xmin / x_step) * x_step;

			for (float val = start_x; val <= f_xmax; val += x_step)
			{
				float centered_val = val + 0.5f;

				if (centered_val < f_xmin || centered_val > f_xmax)
				{
					continue;
				}

				float x_ratio = (f_w > 0) ? (centered_val - f_xmin) / f_w : 0.0f;
				float x_pos = canvas_pos.x + x_ratio * disp_w;
				
				draw_list->AddLine(ImVec2(x_pos, canvas_pos.y + disp_h), ImVec2(x_pos, canvas_pos.y + disp_h + 5), tick_col);
				draw_list->AddLine(ImVec2(x_pos, canvas_pos.y), ImVec2(x_pos, canvas_pos.y - 5), tick_col);
				
				char buf[16]; snprintf(buf, 16, "%g", val);
				ImVec2 ts = ImGui::CalcTextSize(buf);

				draw_list->AddText(ImVec2(x_pos - ts.x * 0.5f, canvas_pos.y + disp_h + 7), tick_col, buf);
				draw_list->AddText(ImVec2(x_pos - ts.x * 0.5f, canvas_pos.y - 20), tick_col, buf);
			}

			float y_step = CalculateNiceStep(f_h, 7); 
			float start_y = std::floor(f_ymin / y_step) * y_step;

			for (float val = start_y; val <= f_ymax; val += y_step)
			{
				float centered_val = val + 0.5f;
				if (centered_val < f_ymin || centered_val > f_ymax)
				{
					continue;
				}

				float y_ratio = (f_h > 0) ? (centered_val - f_ymin) / f_h : 0.0f;
				float y_pos = canvas_pos.y + disp_h - (y_ratio * disp_h);
				
				draw_list->AddLine(ImVec2(canvas_pos.x - 5, y_pos), ImVec2(canvas_pos.x, y_pos), tick_col);
				draw_list->AddLine(ImVec2(canvas_pos.x + disp_w, y_pos), ImVec2(canvas_pos.x + disp_w + 5, y_pos), tick_col);
				
				char buf[16]; snprintf(buf, 16, "%g", val);
				ImVec2 ts = ImGui::CalcTextSize(buf);

				float left_label_x = canvas_pos.x - 10.0f - ts.x;
				draw_list->AddText(ImVec2(left_label_x, y_pos - ts.y * 0.5f), tick_col, buf);
				
				float right_label_x = canvas_pos.x + disp_w + 10.0f;
				draw_list->AddText(ImVec2(right_label_x, y_pos - ts.y * 0.5f), tick_col, buf);
			}
			
			draw_list->AddRect(canvas_pos, ImVec2(canvas_pos.x + disp_w, canvas_pos.y + disp_h), tick_col);

			int fits_w = data.naxis1;
			int fits_h = data.naxis2;
			ImVec2 m_delta = ImGui::GetIO().MouseDelta;

			int n_dims[3] = {data.naxis1, data.naxis2, data.naxis3};

			int cur_nx = n_dims[axis_x];
			int cur_ny = n_dims[axis_y];

			if (ImGui::IsItemHovered()) 
			{
				if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) 
				{
					float fits_units_per_pixel_x = (app_view.x_max - app_view.x_min) / disp_w;
					float fits_units_per_pixel_y = (app_view.y_max - app_view.y_min) / disp_h;
					app_view.x_min -= m_delta.x * fits_units_per_pixel_x;
					app_view.x_max -= m_delta.x * fits_units_per_pixel_x;
					app_view.y_min += m_delta.y * fits_units_per_pixel_y;
					app_view.y_max += m_delta.y * fits_units_per_pixel_y;
					needs_update = true;
				}

				if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) 
				{
					if (!drag_roi.active) 
					{
						ScreenToFitsIdx(ImGui::GetIO().MouseClickedPos[0], &drag_roi.x0, &drag_roi.y0, app_view, canvas_p0, canvas_sz, cur_nx, cur_ny);
						drag_roi.active = true;
						drag_button = 0;
					}
				}

				if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) 
				{
					if (!drag_roi.active) 
					{
						ScreenToFitsIdx(ImGui::GetIO().MouseClickedPos[1], &drag_roi.x0, &drag_roi.y0, app_view, canvas_p0, canvas_sz, cur_nx, cur_ny);
						drag_roi.active = true;
						drag_button = 1;
					}
				}
			}

			if (drag_roi.active) 
			{
				ScreenToFitsIdx(ImGui::GetMousePos(), &drag_roi.x1, &drag_roi.y1, app_view, canvas_p0, canvas_sz, cur_nx, cur_ny);
			}

			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) 
			{
				if (drag_roi.active && drag_button == 0) 
				{
					spectrum = CalculateAverageSpectrum(data, drag_roi, axis_map, n_dims);
					persistent_roi = drag_roi;
					show_persistent_roi = true;
					drag_roi.active = false;
					drag_button = -1;
					needs_update = true;
				} 
				else if (!drag_roi.active && ImGui::IsItemHovered()) 
				{
					ScreenToFitsIdx(ImGui::GetMousePos(), &clicked_ix, &clicked_iy, app_view, canvas_p0, canvas_sz, cur_nx, cur_ny);
					spectrum = ExtractPointSpectrum(data, clicked_ix, clicked_iy, axis_map, n_dims);
					show_persistent_roi = false;
					needs_update = true;
				}
			}

			if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) 
			{
				if (drag_roi.active && drag_button == 1) 
				{
					ApplyZoom(drag_roi, app_view);
					drag_roi.active = false;
					drag_button = -1;
					needs_update = true;
				}
			}

			float wheel = ImGui::GetIO().MouseWheel;

			if (wheel != 0.0f && ImGui::IsItemHovered())
			{
				float zoom_factor = (wheel > 0.0f) ? 0.9f : 1.1f;
				float canvas_aspect = canvas_sz.y / canvas_sz.x;

				float u = std::clamp((ImGui::GetMousePos().x - canvas_p0.x) / canvas_sz.x, 0.0f, 1.0f);
				float v = std::clamp((ImGui::GetMousePos().y - canvas_p0.y) / canvas_sz.y, 0.0f, 1.0f);
				
				float fx = app_view.x_min + u * (app_view.x_max - app_view.x_min);
				float fy = app_view.y_min + (1.0f - v) * (app_view.y_max - app_view.y_min);

				float cur_w = app_view.x_max - app_view.x_min;
				float next_w = cur_w * zoom_factor;

				const float MIN_VISIBLE_WIDTH = 0.5f;
				float data_aspect = (float)cur_ny / (float)cur_nx;
				float max_w = (data_aspect > canvas_aspect) ? ((float)cur_ny / canvas_aspect) : (float)cur_nx;

				next_w = std::clamp(next_w, MIN_VISIBLE_WIDTH, max_w);
				float next_h = next_w * canvas_aspect;

				if (std::abs(next_w - cur_w) > 1e-5f)
				{
					app_view.x_min = fx - u * next_w;
					app_view.x_max = app_view.x_min + next_w;
					app_view.y_min = fy - (1.0f - v) * next_h;
					app_view.y_max = app_view.y_min + next_h;

					needs_update = true;
				}
			}
		}
		ImGui::End();

		ImGui::SetNextWindowPos(ImVec2(v_pos.x + SIDEBAR_W, v_pos.y + main_h));
		ImGui::SetNextWindowSize(ImVec2(main_w, BOTTOM_H));
		ImGui::Begin("Spectrum", nullptr, static_flags);

		auto ScreenToSpecX = [&](ImVec2 mouse_pos, ImVec2 plot_p0, ImVec2 plot_sz, const SpecViewState& vp) -> float
		{
			float u = std::clamp((mouse_pos.x - plot_p0.x) / plot_sz.x, 0.0f, 1.0f);
			return vp.x_min + u * (vp.x_max - vp.x_min);
		};
		
		if (!spectrum.empty())
		{
			ImGui::Separator();
			const char* modes[] = {"Index", "WCS", "LUT"};
			int current_mode = (int)app_spec_view.mode;

			ImGui::AlignTextToFramePadding();
			ImGui::Text("X Axis Mode");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140.0f);

			if (ImGui::Combo("##X-Axis Mode", &current_mode, modes, 3))
			{
				app_spec_view.mode = (XAxisMode)current_mode;
				app_spec_view.initialized = false;
			}

			if(app_spec_view.mode != XAxisMode::Index)
			{
				ImGui::SameLine();
			}
			
			if (app_spec_view.mode == XAxisMode::Index)
			{
				;
			}
			else if (app_spec_view.mode == XAxisMode::WCS)
			{
				int auto_axis = 4 - (axis_x + axis_y);

				if (selected_wcs_axis != auto_axis) 
				{
					selected_wcs_axis = auto_axis;
					UpdateWCSParams(data, selected_wcs_axis);
					app_spec_view.initialized = false;
				}

				ImGui::SameLine();			
				ImGui::Text("CRVAL: %.4e, CDELT: %.4e, CRPIX: %.1f", data.wcs_crval, data.wcs_cdelt, data.wcs_crpix);
			}
			else if (app_spec_view.mode == XAxisMode::Table)
			{
				std::string current_label = active_lut ? data.extensions[selected_hdu_idx].columns[selected_col_idx].name : "Select Column...";
				
				ImGui::SetNextItemWidth(300.0f);
				if (ImGui::BeginCombo("##LUT_Tree", current_label.c_str()))
				{
					int naxes[3] = { data.naxis1, data.naxis2, data.naxis3 };
					int spec_axis_idx = 3 - (axis_x + axis_y);
					
					for (int h = 0; h < (int)data.extensions.size(); ++h)
					{
						auto& hdu = data.extensions[h];
						
						if (ImGui::TreeNode((void*)(intptr_t)h, "HDU %d: %s", h + 1, hdu.name.c_str()))
						{
							for (int c = 0; c < (int)hdu.columns.size(); ++c)
							{
								bool size_match = (hdu.columns[c].data.size() == (size_t)naxes[spec_axis_idx]);
								bool is_selected = (selected_hdu_idx == h && selected_col_idx == c);

								if (!size_match)
								{
									ImGui::BeginDisabled();
								}

								std::string item_label = hdu.columns[c].name + " (" + std::to_string(hdu.columns[c].data.size()) + ")";

								if (ImGui::Selectable(item_label.c_str(), is_selected))
								{
									selected_hdu_idx = h;
									selected_col_idx = c;
									active_lut = &hdu.columns[c].data;

									if (active_lut && !active_lut->empty())
									{
										app_spec_view.x_min = active_lut->front();
										app_spec_view.x_max = active_lut->back();
									}

									app_spec_view.initialized = false;
								}

								if (!size_match)
								{
									ImGui::EndDisabled();

									if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
									{
										ImGui::SetTooltip("Dimension mismatch. Expected: %d", data.naxis1);
									}
								}
							}
							ImGui::TreePop();
						}
					}
					ImGui::EndCombo();
				}
			}

			size_t spec_size = spectrum.size();
			spectral_grid.resize(spec_size);

			if (app_spec_view.mode == XAxisMode::WCS)
			{
				for (size_t i = 0; i < spec_size; ++i)
				{
					spectral_grid[i] = data.wcs_crval + ((float)i + 1.0f - data.wcs_crpix) * data.wcs_cdelt;
				}
			}
			else if (app_spec_view.mode == XAxisMode::Table && active_lut)
			{
				if (active_lut->size() == spec_size)
				{
					spectral_grid = *active_lut; 
				}
				else
				{
					for (size_t i = 0; i < spec_size; ++i)
					{
						spectral_grid[i] = (float)i;
					}
				}
			}
			else
			{
				for (size_t i = 0; i < spec_size; ++i)
				{
					spectral_grid[i] = (float)i;
				}
			}
			
			if (!app_spec_view.initialized)
			{
				ResetSpecView(app_spec_view, spectrum, spectral_grid);
			}
			
			if (current_tool == ToolMode::Average || show_persistent_roi)
			{
				const ROI& display_roi = drag_roi.active ? drag_roi : persistent_roi;

				ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "ROI Average:");
				ImGui::SameLine();

				int xmin = std::min(display_roi.x0, display_roi.x1);
				int xmax = std::max(display_roi.x0, display_roi.x1);
				int ymin = std::min(display_roi.y0, display_roi.y1);
				int ymax = std::max(display_roi.y0, display_roi.y1);

				ImGui::Text("[%d:%d, %d:%d]", xmin, xmax, ymin, ymax);
			}
			else
			{
				ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Point Probe:");
				ImGui::SameLine();
				ImGui::Text("x=%d, y=%d", clicked_ix, clicked_iy);
			}

			char range_buf[64];
			snprintf(range_buf, 64, "Range: %.1f - %.1f", app_spec_view.x_min, app_spec_view.x_max);
			
			float text_width = ImGui::CalcTextSize(range_buf).x;
			float button_width = 80.0f;
			float spacing = ImGui::GetStyle().ItemSpacing.x;
			float padding = ImGui::GetStyle().WindowPadding.x;

			float total_needed_width = text_width + spacing + button_width + padding;
			ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - total_needed_width));

			ImGui::AlignTextToFramePadding();
			ImGui::Text("%s", range_buf);
			ImGui::SameLine();

			if (ImGui::Button("Reset", ImVec2(button_width, 0)))
			{
				ResetSpecView(app_spec_view, spectrum, spectral_grid);
			}
			
			if (!app_spec_view.initialized && !spectral_grid.empty())
			{
				app_spec_view.x_min = spectral_grid.front();
				app_spec_view.x_max = spectral_grid.back();
				app_spec_view.initialized = true;
			}

			ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
			ImVec2 canvas_sz = ImVec2(ImGui::GetContentRegionAvail().x, 220);

			const float m_left = 80.0f;
			const float m_right = 20.0f;
			const float m_top = 10.0f;
			const float m_bottom = 30.0f;

			ImVec2 plot_p0 = ImVec2(canvas_p0.x + m_left, canvas_p0.y + m_top);
			ImVec2 plot_sz = ImVec2(canvas_sz.x - (m_left + m_right), canvas_sz.y - (m_top + m_bottom));

			auto ScreenToSpecX = [](ImVec2 pos, ImVec2 p0, ImVec2 sz, const SpecViewState& vp) -> float
			{
				float u = std::clamp((pos.x - p0.x) / sz.x, 0.0f, 1.0f);
				return vp.x_min + u * (vp.x_max - vp.x_min);
			};

			ImGui::InvisibleButton("spec_interaction", canvas_sz);
			bool is_hovered = ImGui::IsItemHovered();

			if (is_hovered)
			{
				if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
				{
					if (!spec_dragging)
					{
						spec_drag_start_x = ScreenToSpecX(ImGui::GetIO().MouseClickedPos[1], plot_p0, plot_sz, app_spec_view);
						spec_dragging = true;
					}

					spec_drag_current_x = ScreenToSpecX(ImGui::GetMousePos(), plot_p0, plot_sz, app_spec_view);
				}

				if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && spec_dragging)
				{
					float x0 = std::min(spec_drag_start_x, spec_drag_current_x);
					float x1 = std::max(spec_drag_start_x, spec_drag_current_x);
					float drag_dist_px = std::abs(ImGui::GetMousePos().x - ImGui::GetIO().MouseClickedPos[1].x);

					if (drag_dist_px > 3.0f)
					{
						app_spec_view.x_min = x0;
						app_spec_view.x_max = x1;
					}
					spec_dragging = false;
				}

				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Right))
				{
					ResetSpecView(app_spec_view, spectrum, spectral_grid);
				}
			}

			ImGui::SetCursorScreenPos(canvas_p0);
			
			if (!spectrum.empty())
			{
				auto it_s = std::lower_bound(spectral_grid.begin(), spectral_grid.end(), app_spec_view.x_min);
				auto it_e = std::lower_bound(spectral_grid.begin(), spectral_grid.end(), app_spec_view.x_max);
				
				int start_i = (int)std::distance(spectral_grid.begin(), it_s);
				int end_i   = (int)std::distance(spectral_grid.begin(), it_e);

				if (start_i > end_i)
				{
					std::swap(start_i, end_i);
				}

				start_i = std::clamp(start_i, 0, (int)spectrum.size() - 1);
				end_i   = std::clamp(end_i, 0, (int)spectrum.size() - 1);

				float v_min = FLT_MAX;
				float v_max = -FLT_MAX;

				for (int i = start_i; i <= end_i; i++)
				{
					if (spectrum[i] < v_min) v_min = spectrum[i];
					if (spectrum[i] > v_max) v_max = spectrum[i];
				}

				if (v_min == v_max || v_min == FLT_MAX)
				{
					v_min -= 1.0f;
					v_max += 1.0f;
				}
				else
				{
					float pad = (v_max - v_min) * 0.1f;
					v_min -= pad;
					v_max += pad;
				}

				app_spec_view.y_min = v_min;
				app_spec_view.y_max = v_max;
			}

			std::cout << "spectral grid" << std::endl;
			for(int i = 0; i < spectral_grid.size(); ++i)
			{
				std::cout << spectral_grid[i] << std::endl;
			}
			std::cout << "end" << std::endl;
			DrawCustomSpectrum(spectrum, spectral_grid, app_spec_view.x_min, app_spec_view.x_max, app_spec_view.y_min, app_spec_view.y_max, current_slice);

			if (spec_dragging) 
			{
				ImDrawList* draw_list = ImGui::GetWindowDrawList();
				float view_w = app_spec_view.x_max - app_spec_view.x_min;
				float px0 = plot_p0.x + (std::min(spec_drag_start_x, spec_drag_current_x) - app_spec_view.x_min) / view_w * plot_sz.x;
				float px1 = plot_p0.x + (std::max(spec_drag_start_x, spec_drag_current_x) - app_spec_view.x_min) / view_w * plot_sz.x;
				
				draw_list->AddRectFilled(ImVec2(px0, plot_p0.y), ImVec2(px1, plot_p0.y + plot_sz.y), IM_COL32(60, 150, 255, 60));
				draw_list->AddLine(ImVec2(px0, plot_p0.y), ImVec2(px0, plot_p0.y + plot_sz.y), IM_COL32(60, 150, 255, 200));
				draw_list->AddLine(ImVec2(px1, plot_p0.y), ImVec2(px1, plot_p0.y + plot_sz.y), IM_COL32(60, 150, 255, 200));
			}
		}
		else
		{
			ImGui::TextDisabled("Click or drag on the image to analyze data.");
		}

		ImGui::End();
		
		ImGui::Render();
		int display_w, display_h;
		glfwGetFramebufferSize(window, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		glfwSwapBuffers(window);
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(window);
	glfwTerminate();

	return 0;
}
