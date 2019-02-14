/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// [[CITE]] ABC
// Berkeley Logic Synthesis and Verification Group, ABC: A System for Sequential Synthesis and Verification
// http://www.eecs.berkeley.edu/~alanmi/abc/

// [[CITE]] Berkeley Logic Interchange Format (BLIF)
// University of California. Berkeley. July 28, 1992
// http://www.ece.cmu.edu/~ee760/760docs/blif.pdf

// [[CITE]] Kahn's Topological sorting algorithm
// Kahn, Arthur B. (1962), "Topological sorting of large networks", Communications of the ACM 5 (11): 558-562, doi:10.1145/368996.369025
// http://en.wikipedia.org/wiki/Topological_sorting

#define ABC_COMMAND_LIB "strash; ifraig; scorr; dc2; dretime; strash; &get -n; &dch -f; &nf {D}; &put"
#define ABC_COMMAND_CTR "strash; ifraig; scorr; dc2; dretime; strash; &get -n; &dch -f; &nf {D}; &put; buffer; upsize {D}; dnsize {D}; stime -p"
#define ABC_COMMAND_LUT "&st; &fraig; &scorr; &dc2; &retime; &dch -f; &if;"/*" &mfs"*/
#define ABC_COMMAND_SOP "strash; ifraig; scorr; dc2; dretime; strash; dch -f; cover {I} {P}"
#define ABC_COMMAND_DFL "strash; ifraig; scorr; dc2; dretime; strash; &get -n; &dch -f; &nf {D}; &put"

#define ABC_FAST_COMMAND_LIB "strash; dretime; map {D}"
#define ABC_FAST_COMMAND_CTR "strash; dretime; map {D}; buffer; upsize {D}; dnsize {D}; stime -p"
#define ABC_FAST_COMMAND_LUT "strash; dretime; if"
#define ABC_FAST_COMMAND_SOP "strash; dretime; cover -I {I} -P {P}"
#define ABC_FAST_COMMAND_DFL "strash; dretime; map"

#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "kernel/cost.h"
#include "kernel/log.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <cerrno>
#include <sstream>
#include <climits>

#ifndef _WIN32
#  include <unistd.h>
#  include <dirent.h>
#endif

#include "frontends/aiger/aigerparse.h"

#ifdef YOSYS_LINK_ABC
extern "C" int Abc_RealMain(int argc, char *argv[]);
#endif

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

bool map_mux4;
bool map_mux8;
bool map_mux16;

bool markgroups;
int map_autoidx;
SigMap assign_map;
RTLIL::Module *module;
std::map<RTLIL::SigBit, int> signal_map;
std::map<RTLIL::SigBit, RTLIL::State> signal_init;
pool<std::string> enabled_gates;
bool recover_init;

bool clk_polarity, en_polarity;
RTLIL::SigSpec clk_sig, en_sig;
dict<int, std::string> pi_map, po_map;

std::string remap_name(RTLIL::IdString abc_name)
{
	std::stringstream sstr;
	sstr << "$abc$" << map_autoidx << "$" << abc_name.substr(1);
	return sstr.str();
}

std::string add_echos_to_abc_cmd(std::string str)
{
	std::string new_str, token;
	for (size_t i = 0; i < str.size(); i++) {
		token += str[i];
		if (str[i] == ';') {
			while (i+1 < str.size() && str[i+1] == ' ')
				i++;
			new_str += "echo + " + token + " " + token + " ";
			token.clear();
		}
	}

	if (!token.empty()) {
		if (!new_str.empty())
			new_str += "echo + " + token + "; ";
		new_str += token;
	}

	return new_str;
}

std::string fold_abc_cmd(std::string str)
{
	std::string token, new_str = "          ";
	int char_counter = 10;

	for (size_t i = 0; i <= str.size(); i++) {
		if (i < str.size())
			token += str[i];
		if (i == str.size() || str[i] == ';') {
			if (char_counter + token.size() > 75)
				new_str += "\n              ", char_counter = 14;
			new_str += token, char_counter += token.size();
			token.clear();
		}
	}

	return new_str;
}

std::string replace_tempdir(std::string text, std::string tempdir_name, bool show_tempdir)
{
	if (show_tempdir)
		return text;

	while (1) {
		size_t pos = text.find(tempdir_name);
		if (pos == std::string::npos)
			break;
		text = text.substr(0, pos) + "<abc-temp-dir>" + text.substr(pos + GetSize(tempdir_name));
	}

	std::string  selfdir_name = proc_self_dirname();
	if (selfdir_name != "/") {
		while (1) {
			size_t pos = text.find(selfdir_name);
			if (pos == std::string::npos)
				break;
			text = text.substr(0, pos) + "<yosys-exe-dir>/" + text.substr(pos + GetSize(selfdir_name));
		}
	}

	return text;
}

struct abc_output_filter
{
	bool got_cr;
	int escape_seq_state;
	std::string linebuf;
	std::string tempdir_name;
	bool show_tempdir;

	abc_output_filter(std::string tempdir_name, bool show_tempdir) : tempdir_name(tempdir_name), show_tempdir(show_tempdir)
	{
		got_cr = false;
		escape_seq_state = 0;
	}

	void next_char(char ch)
	{
		if (escape_seq_state == 0 && ch == '\033') {
			escape_seq_state = 1;
			return;
		}
		if (escape_seq_state == 1) {
			escape_seq_state = ch == '[' ? 2 : 0;
			return;
		}
		if (escape_seq_state == 2) {
			if ((ch < '0' || '9' < ch) && ch != ';')
				escape_seq_state = 0;
			return;
		}
		escape_seq_state = 0;
		if (ch == '\r') {
			got_cr = true;
			return;
		}
		if (ch == '\n') {
			log("ABC: %s\n", replace_tempdir(linebuf, tempdir_name, show_tempdir).c_str());
			got_cr = false, linebuf.clear();
			return;
		}
		if (got_cr)
			got_cr = false, linebuf.clear();
		linebuf += ch;
	}

	void next_line(const std::string &line)
	{
		int pi, po;
		if (sscanf(line.c_str(), "Start-point = pi%d.  End-point = po%d.", &pi, &po) == 2) {
			log("ABC: Start-point = pi%d (%s).  End-point = po%d (%s).\n",
					pi, pi_map.count(pi) ? pi_map.at(pi).c_str() : "???",
					po, po_map.count(po) ? po_map.at(po).c_str() : "???");
			return;
		}

		for (char ch : line)
			next_char(ch);
	}
};

void abc9_module(RTLIL::Design *design, RTLIL::Module *current_module, std::string script_file, std::string exe_file,
		std::string liberty_file, std::string constr_file, bool cleanup, vector<int> lut_costs, bool dff_mode, std::string clk_str,
		bool keepff, std::string delay_target, std::string sop_inputs, std::string sop_products, std::string lutin_shared, bool fast_mode,
		const std::vector<RTLIL::Cell*> &cells, bool show_tempdir, bool sop_mode)
{
	module = current_module;
	map_autoidx = autoidx++;

	signal_map.clear();
	pi_map.clear();
	po_map.clear();
	recover_init = false;

	if (clk_str != "$")
	{
		clk_polarity = true;
		clk_sig = RTLIL::SigSpec();

		en_polarity = true;
		en_sig = RTLIL::SigSpec();
	}

	if (!clk_str.empty() && clk_str != "$")
	{
		if (clk_str.find(',') != std::string::npos) {
			int pos = clk_str.find(',');
			std::string en_str = clk_str.substr(pos+1);
			clk_str = clk_str.substr(0, pos);
			if (en_str[0] == '!') {
				en_polarity = false;
				en_str = en_str.substr(1);
			}
			if (module->wires_.count(RTLIL::escape_id(en_str)) != 0)
				en_sig = assign_map(RTLIL::SigSpec(module->wires_.at(RTLIL::escape_id(en_str)), 0));
		}
		if (clk_str[0] == '!') {
			clk_polarity = false;
			clk_str = clk_str.substr(1);
		}
		if (module->wires_.count(RTLIL::escape_id(clk_str)) != 0)
			clk_sig = assign_map(RTLIL::SigSpec(module->wires_.at(RTLIL::escape_id(clk_str)), 0));
	}

	if (dff_mode && clk_sig.empty())
		log_cmd_error("Clock domain %s not found.\n", clk_str.c_str());

	std::string tempdir_name = "/tmp/yosys-abc-XXXXXX";
	if (!cleanup)
		tempdir_name[0] = tempdir_name[4] = '_';
	tempdir_name = make_temp_dir(tempdir_name);
	log_header(design, "Extracting gate netlist of module `%s' to `%s/input.xaig'..\n",
			module->name.c_str(), replace_tempdir(tempdir_name, tempdir_name, show_tempdir).c_str());

	std::string abc_script = stringf("&read %s/input.xaig; &ps; ", tempdir_name.c_str());

	if (!liberty_file.empty()) {
		abc_script += stringf("read_lib -w %s; ", liberty_file.c_str());
		if (!constr_file.empty())
			abc_script += stringf("read_constr -v %s; ", constr_file.c_str());
	} else
	if (!lut_costs.empty())
		abc_script += stringf("read_lut %s/lutdefs.txt; ", tempdir_name.c_str());
	else
		abc_script += stringf("read_library %s/stdcells.genlib; ", tempdir_name.c_str());

	if (!script_file.empty()) {
		if (script_file[0] == '+') {
			for (size_t i = 1; i < script_file.size(); i++)
				if (script_file[i] == '\'')
					abc_script += "'\\''";
				else if (script_file[i] == ',')
					abc_script += " ";
				else
					abc_script += script_file[i];
		} else
			abc_script += stringf("source %s", script_file.c_str());
	} else if (!lut_costs.empty()) {
		bool all_luts_cost_same = true;
		for (int this_cost : lut_costs)
			if (this_cost != lut_costs.front())
				all_luts_cost_same = false;
		abc_script += fast_mode ? ABC_FAST_COMMAND_LUT : ABC_COMMAND_LUT;
		//if (all_luts_cost_same && !fast_mode)
		//	abc_script += "; lutpack {S}";
	} else if (!liberty_file.empty())
		abc_script += constr_file.empty() ? (fast_mode ? ABC_FAST_COMMAND_LIB : ABC_COMMAND_LIB) : (fast_mode ? ABC_FAST_COMMAND_CTR : ABC_COMMAND_CTR);
	else if (sop_mode)
		abc_script += fast_mode ? ABC_FAST_COMMAND_SOP : ABC_COMMAND_SOP;
	else
		abc_script += fast_mode ? ABC_FAST_COMMAND_DFL : ABC_COMMAND_DFL;

	if (script_file.empty() && !delay_target.empty())
		for (size_t pos = abc_script.find("dretime;"); pos != std::string::npos; pos = abc_script.find("dretime;", pos+1))
			abc_script = abc_script.substr(0, pos) + "dretime; retime -o {D};" + abc_script.substr(pos+8);

	for (size_t pos = abc_script.find("{D}"); pos != std::string::npos; pos = abc_script.find("{D}", pos))
		abc_script = abc_script.substr(0, pos) + delay_target + abc_script.substr(pos+3);

	for (size_t pos = abc_script.find("{I}"); pos != std::string::npos; pos = abc_script.find("{D}", pos))
		abc_script = abc_script.substr(0, pos) + sop_inputs + abc_script.substr(pos+3);

	for (size_t pos = abc_script.find("{P}"); pos != std::string::npos; pos = abc_script.find("{D}", pos))
		abc_script = abc_script.substr(0, pos) + sop_products + abc_script.substr(pos+3);

	for (size_t pos = abc_script.find("{S}"); pos != std::string::npos; pos = abc_script.find("{S}", pos))
		abc_script = abc_script.substr(0, pos) + lutin_shared + abc_script.substr(pos+3);

	abc_script += stringf("; &ps; &write -v %s/output.xaig", tempdir_name.c_str());
	abc_script = add_echos_to_abc_cmd(abc_script);

	for (size_t i = 0; i+1 < abc_script.size(); i++)
		if (abc_script[i] == ';' && abc_script[i+1] == ' ')
			abc_script[i+1] = '\n';

	FILE *f = fopen(stringf("%s/abc.script", tempdir_name.c_str()).c_str(), "wt");
	fprintf(f, "%s\n", abc_script.c_str());
	fclose(f);

	if (dff_mode || !clk_str.empty())
	{
		if (clk_sig.size() == 0)
			log("No%s clock domain found. Not extracting any FF cells.\n", clk_str.empty() ? "" : " matching");
		else {
			log("Found%s %s clock domain: %s", clk_str.empty() ? "" : " matching", clk_polarity ? "posedge" : "negedge", log_signal(clk_sig));
			if (en_sig.size() != 0)
				log(", enabled by %s%s", en_polarity ? "" : "!", log_signal(en_sig));
			log("\n");
		}
	}

    Pass::call(design, stringf("aigmap; write_xaiger -map %s/input.symbols %s/input.xaig; ", tempdir_name.c_str(), tempdir_name.c_str()));

	log_push();

	//if (count_output > 0)
	{
		log_header(design, "Executing ABC.\n");

		std::string buffer = stringf("%s/stdcells.genlib", tempdir_name.c_str());
		f = fopen(buffer.c_str(), "wt");
		if (f == NULL)
			log_error("Opening %s for writing failed: %s\n", buffer.c_str(), strerror(errno));
		fprintf(f, "GATE ZERO    1 Y=CONST0;\n");
		fprintf(f, "GATE ONE     1 Y=CONST1;\n");
		fprintf(f, "GATE BUF    %d Y=A;                  PIN * NONINV  1 999 1 0 1 0\n", get_cell_cost("$_BUF_"));
		fprintf(f, "GATE NOT    %d Y=!A;                 PIN * INV     1 999 1 0 1 0\n", get_cell_cost("$_NOT_"));
		if (enabled_gates.empty() || enabled_gates.count("AND"))
			fprintf(f, "GATE AND    %d Y=A*B;                PIN * NONINV  1 999 1 0 1 0\n", get_cell_cost("$_AND_"));
		if (enabled_gates.empty() || enabled_gates.count("NAND"))
			fprintf(f, "GATE NAND   %d Y=!(A*B);             PIN * INV     1 999 1 0 1 0\n", get_cell_cost("$_NAND_"));
		if (enabled_gates.empty() || enabled_gates.count("OR"))
			fprintf(f, "GATE OR     %d Y=A+B;                PIN * NONINV  1 999 1 0 1 0\n", get_cell_cost("$_OR_"));
		if (enabled_gates.empty() || enabled_gates.count("NOR"))
			fprintf(f, "GATE NOR    %d Y=!(A+B);             PIN * INV     1 999 1 0 1 0\n", get_cell_cost("$_NOR_"));
		if (enabled_gates.empty() || enabled_gates.count("XOR"))
			fprintf(f, "GATE XOR    %d Y=(A*!B)+(!A*B);      PIN * UNKNOWN 1 999 1 0 1 0\n", get_cell_cost("$_XOR_"));
		if (enabled_gates.empty() || enabled_gates.count("XNOR"))
			fprintf(f, "GATE XNOR   %d Y=(A*B)+(!A*!B);      PIN * UNKNOWN 1 999 1 0 1 0\n", get_cell_cost("$_XNOR_"));
		if (enabled_gates.empty() || enabled_gates.count("ANDNOT"))
			fprintf(f, "GATE ANDNOT %d Y=A*!B;               PIN * UNKNOWN 1 999 1 0 1 0\n", get_cell_cost("$_ANDNOT_"));
		if (enabled_gates.empty() || enabled_gates.count("ORNOT"))
			fprintf(f, "GATE ORNOT  %d Y=A+!B;               PIN * UNKNOWN 1 999 1 0 1 0\n", get_cell_cost("$_ORNOT_"));
		if (enabled_gates.empty() || enabled_gates.count("AOI3"))
			fprintf(f, "GATE AOI3   %d Y=!((A*B)+C);         PIN * INV     1 999 1 0 1 0\n", get_cell_cost("$_AOI3_"));
		if (enabled_gates.empty() || enabled_gates.count("OAI3"))
			fprintf(f, "GATE OAI3   %d Y=!((A+B)*C);         PIN * INV     1 999 1 0 1 0\n", get_cell_cost("$_OAI3_"));
		if (enabled_gates.empty() || enabled_gates.count("AOI4"))
			fprintf(f, "GATE AOI4   %d Y=!((A*B)+(C*D));     PIN * INV     1 999 1 0 1 0\n", get_cell_cost("$_AOI4_"));
		if (enabled_gates.empty() || enabled_gates.count("OAI4"))
			fprintf(f, "GATE OAI4   %d Y=!((A+B)*(C+D));     PIN * INV     1 999 1 0 1 0\n", get_cell_cost("$_OAI4_"));
		if (enabled_gates.empty() || enabled_gates.count("MUX"))
			fprintf(f, "GATE MUX    %d Y=(A*B)+(S*B)+(!S*A); PIN * UNKNOWN 1 999 1 0 1 0\n", get_cell_cost("$_MUX_"));
		if (map_mux4)
			fprintf(f, "GATE MUX4   %d Y=(!S*!T*A)+(S*!T*B)+(!S*T*C)+(S*T*D); PIN * UNKNOWN 1 999 1 0 1 0\n", 2*get_cell_cost("$_MUX_"));
		if (map_mux8)
			fprintf(f, "GATE MUX8   %d Y=(!S*!T*!U*A)+(S*!T*!U*B)+(!S*T*!U*C)+(S*T*!U*D)+(!S*!T*U*E)+(S*!T*U*F)+(!S*T*U*G)+(S*T*U*H); PIN * UNKNOWN 1 999 1 0 1 0\n", 4*get_cell_cost("$_MUX_"));
		if (map_mux16)
			fprintf(f, "GATE MUX16  %d Y=(!S*!T*!U*!V*A)+(S*!T*!U*!V*B)+(!S*T*!U*!V*C)+(S*T*!U*!V*D)+(!S*!T*U*!V*E)+(S*!T*U*!V*F)+(!S*T*U*!V*G)+(S*T*U*!V*H)+(!S*!T*!U*V*I)+(S*!T*!U*V*J)+(!S*T*!U*V*K)+(S*T*!U*V*L)+(!S*!T*U*V*M)+(S*!T*U*V*N)+(!S*T*U*V*O)+(S*T*U*V*P); PIN * UNKNOWN 1 999 1 0 1 0\n", 8*get_cell_cost("$_MUX_"));
		fclose(f);

		if (!lut_costs.empty()) {
			buffer = stringf("%s/lutdefs.txt", tempdir_name.c_str());
			f = fopen(buffer.c_str(), "wt");
			if (f == NULL)
				log_error("Opening %s for writing failed: %s\n", buffer.c_str(), strerror(errno));
			for (int i = 0; i < GetSize(lut_costs); i++)
				fprintf(f, "%d %d.00 1.00\n", i+1, lut_costs.at(i));
			fclose(f);
		}

		buffer = stringf("%s -s -f %s/abc.script 2>&1", exe_file.c_str(), tempdir_name.c_str());
		log("Running ABC command: %s\n", replace_tempdir(buffer, tempdir_name, show_tempdir).c_str());

#ifndef YOSYS_LINK_ABC
		abc_output_filter filt(tempdir_name, show_tempdir);
		int ret = run_command(buffer, std::bind(&abc_output_filter::next_line, filt, std::placeholders::_1));
#else
		// These needs to be mutable, supposedly due to getopt
		char *abc_argv[5];
		string tmp_script_name = stringf("%s/abc.script", tempdir_name.c_str());
		abc_argv[0] = strdup(exe_file.c_str());
		abc_argv[1] = strdup("-s");
		abc_argv[2] = strdup("-f");
		abc_argv[3] = strdup(tmp_script_name.c_str());
		abc_argv[4] = 0;
		int ret = Abc_RealMain(4, abc_argv);
		free(abc_argv[0]);
		free(abc_argv[1]);
		free(abc_argv[2]);
		free(abc_argv[3]);
#endif
		if (ret != 0)
			log_error("ABC: execution of command \"%s\" failed: return code %d.\n", buffer.c_str(), ret);

		buffer = stringf("%s/%s", tempdir_name.c_str(), "output.xaig");
		std::ifstream ifs;
		ifs.open(buffer);
		if (ifs.fail())
			log_error("Can't open ABC output file `%s'.\n", buffer.c_str());

		bool builtin_lib = liberty_file.empty();
		RTLIL::Design *mapped_design = new RTLIL::Design;
		//parse_blif(mapped_design, ifs, builtin_lib ? "\\DFF" : "\\_dff_", false, sop_mode);
		buffer = stringf("%s/%s", tempdir_name.c_str(), "input.symbols");
		AigerReader reader(mapped_design, ifs, "\\netlist", "\\clk", buffer, true /* wideports */);
		reader.parse_xaiger();

		ifs.close();

		log_header(design, "Re-integrating ABC results.\n");
		RTLIL::Module *mapped_mod = mapped_design->modules_["\\netlist"];
		if (mapped_mod == NULL)
			log_error("ABC output file does not contain a module `netlist'.\n");
		for (auto &it : mapped_mod->wires_) {
			RTLIL::Wire *w = it.second;
			RTLIL::Wire *wire = module->addWire(remap_name(w->name), GetSize(w));
			if (markgroups) wire->attributes["\\abcgroup"] = map_autoidx;
			design->select(module, wire);
		}

		std::map<std::string, int> cell_stats;
		for (auto c : mapped_mod->cells())
		{
			if (builtin_lib)
			{
				cell_stats[RTLIL::unescape_id(c->type)]++;
				if (c->type == "\\ZERO" || c->type == "\\ONE") {
					RTLIL::SigSig conn;
					conn.first = RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)]);
					conn.second = RTLIL::SigSpec(c->type == "\\ZERO" ? 0 : 1, 1);
					module->connect(conn);
					continue;
				}
				if (c->type == "\\BUF") {
					RTLIL::SigSig conn;
					conn.first = RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)]);
					conn.second = RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\A").as_wire()->name)]);
					module->connect(conn);
					continue;
				}
				if (c->type == "\\NOT") {
					RTLIL::Cell *cell = module->addCell(remap_name(c->name), "$_NOT_");
					if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
					cell->setPort("\\A", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\A").as_wire()->name)]));
					cell->setPort("\\Y", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)]));
					design->select(module, cell);
					continue;
				}
				if (c->type == "\\AND" || c->type == "\\OR" || c->type == "\\XOR" || c->type == "\\NAND" || c->type == "\\NOR" ||
						c->type == "\\XNOR" || c->type == "\\ANDNOT" || c->type == "\\ORNOT") {
					RTLIL::Cell *cell = module->addCell(remap_name(c->name), "$_" + c->type.substr(1) + "_");
					if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
					cell->setPort("\\A", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\A").as_wire()->name)]));
					cell->setPort("\\B", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\B").as_wire()->name)]));
					cell->setPort("\\Y", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)]));
					design->select(module, cell);
					continue;
				}
				if (c->type == "\\MUX") {
					RTLIL::Cell *cell = module->addCell(remap_name(c->name), "$_MUX_");
					if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
					cell->setPort("\\A", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\A").as_wire()->name)]));
					cell->setPort("\\B", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\B").as_wire()->name)]));
					cell->setPort("\\S", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\S").as_wire()->name)]));
					cell->setPort("\\Y", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)]));
					design->select(module, cell);
					continue;
				}
				if (c->type == "\\MUX4") {
					RTLIL::Cell *cell = module->addCell(remap_name(c->name), "$_MUX4_");
					if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
					cell->setPort("\\A", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\A").as_wire()->name)]));
					cell->setPort("\\B", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\B").as_wire()->name)]));
					cell->setPort("\\C", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\C").as_wire()->name)]));
					cell->setPort("\\D", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\D").as_wire()->name)]));
					cell->setPort("\\S", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\S").as_wire()->name)]));
					cell->setPort("\\T", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\T").as_wire()->name)]));
					cell->setPort("\\Y", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)]));
					design->select(module, cell);
					continue;
				}
				if (c->type == "\\MUX8") {
					RTLIL::Cell *cell = module->addCell(remap_name(c->name), "$_MUX8_");
					if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
					cell->setPort("\\A", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\A").as_wire()->name)]));
					cell->setPort("\\B", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\B").as_wire()->name)]));
					cell->setPort("\\C", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\C").as_wire()->name)]));
					cell->setPort("\\D", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\D").as_wire()->name)]));
					cell->setPort("\\E", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\E").as_wire()->name)]));
					cell->setPort("\\F", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\F").as_wire()->name)]));
					cell->setPort("\\G", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\G").as_wire()->name)]));
					cell->setPort("\\H", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\H").as_wire()->name)]));
					cell->setPort("\\S", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\S").as_wire()->name)]));
					cell->setPort("\\T", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\T").as_wire()->name)]));
					cell->setPort("\\U", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\U").as_wire()->name)]));
					cell->setPort("\\Y", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)]));
					design->select(module, cell);
					continue;
				}
				if (c->type == "\\MUX16") {
					RTLIL::Cell *cell = module->addCell(remap_name(c->name), "$_MUX16_");
					if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
					cell->setPort("\\A", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\A").as_wire()->name)]));
					cell->setPort("\\B", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\B").as_wire()->name)]));
					cell->setPort("\\C", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\C").as_wire()->name)]));
					cell->setPort("\\D", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\D").as_wire()->name)]));
					cell->setPort("\\E", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\E").as_wire()->name)]));
					cell->setPort("\\F", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\F").as_wire()->name)]));
					cell->setPort("\\G", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\G").as_wire()->name)]));
					cell->setPort("\\H", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\H").as_wire()->name)]));
					cell->setPort("\\I", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\I").as_wire()->name)]));
					cell->setPort("\\J", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\J").as_wire()->name)]));
					cell->setPort("\\K", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\K").as_wire()->name)]));
					cell->setPort("\\L", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\L").as_wire()->name)]));
					cell->setPort("\\M", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\M").as_wire()->name)]));
					cell->setPort("\\N", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\N").as_wire()->name)]));
					cell->setPort("\\O", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\O").as_wire()->name)]));
					cell->setPort("\\P", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\P").as_wire()->name)]));
					cell->setPort("\\S", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\S").as_wire()->name)]));
					cell->setPort("\\T", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\T").as_wire()->name)]));
					cell->setPort("\\U", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\U").as_wire()->name)]));
					cell->setPort("\\V", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\V").as_wire()->name)]));
					cell->setPort("\\Y", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)]));
					design->select(module, cell);
					continue;
				}
				if (c->type == "\\AOI3" || c->type == "\\OAI3") {
					RTLIL::Cell *cell = module->addCell(remap_name(c->name), "$_" + c->type.substr(1) + "_");
					if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
					cell->setPort("\\A", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\A").as_wire()->name)]));
					cell->setPort("\\B", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\B").as_wire()->name)]));
					cell->setPort("\\C", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\C").as_wire()->name)]));
					cell->setPort("\\Y", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)]));
					design->select(module, cell);
					continue;
				}
				if (c->type == "\\AOI4" || c->type == "\\OAI4") {
					RTLIL::Cell *cell = module->addCell(remap_name(c->name), "$_" + c->type.substr(1) + "_");
					if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
					cell->setPort("\\A", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\A").as_wire()->name)]));
					cell->setPort("\\B", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\B").as_wire()->name)]));
					cell->setPort("\\C", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\C").as_wire()->name)]));
					cell->setPort("\\D", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\D").as_wire()->name)]));
					cell->setPort("\\Y", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)]));
					design->select(module, cell);
					continue;
				}
				if (c->type == "\\DFF") {
					log_assert(clk_sig.size() == 1);
					RTLIL::Cell *cell;
					if (en_sig.size() == 0) {
						cell = module->addCell(remap_name(c->name), clk_polarity ? "$_DFF_P_" : "$_DFF_N_");
					} else {
						log_assert(en_sig.size() == 1);
						cell = module->addCell(remap_name(c->name), stringf("$_DFFE_%c%c_", clk_polarity ? 'P' : 'N', en_polarity ? 'P' : 'N'));
						cell->setPort("\\E", en_sig);
					}
					if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
					cell->setPort("\\D", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\D").as_wire()->name)]));
					cell->setPort("\\Q", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Q").as_wire()->name)]));
					cell->setPort("\\C", clk_sig);
					design->select(module, cell);
					continue;
				}
			}

			cell_stats[RTLIL::unescape_id(c->type)]++;

			if (c->type == "\\_const0_" || c->type == "\\_const1_") {
				RTLIL::SigSig conn;
				conn.first = RTLIL::SigSpec(module->wires_[remap_name(c->connections().begin()->second.as_wire()->name)]);
				conn.second = RTLIL::SigSpec(c->type == "\\_const0_" ? 0 : 1, 1);
				module->connect(conn);
				continue;
			}

			if (c->type == "\\_dff_") {
				log_assert(clk_sig.size() == 1);
				RTLIL::Cell *cell;
				if (en_sig.size() == 0) {
					cell = module->addCell(remap_name(c->name), clk_polarity ? "$_DFF_P_" : "$_DFF_N_");
				} else {
					log_assert(en_sig.size() == 1);
					cell = module->addCell(remap_name(c->name), stringf("$_DFFE_%c%c_", clk_polarity ? 'P' : 'N', en_polarity ? 'P' : 'N'));
					cell->setPort("\\E", en_sig);
				}
				if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
				cell->setPort("\\D", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\D").as_wire()->name)]));
				cell->setPort("\\Q", RTLIL::SigSpec(module->wires_[remap_name(c->getPort("\\Q").as_wire()->name)]));
				cell->setPort("\\C", clk_sig);
				design->select(module, cell);
				continue;
			}

			if (c->type == "$lut" && GetSize(c->getPort("\\A")) == 1 && c->getParam("\\LUT").as_int() == 2) {
				SigSpec my_a = module->wires_[remap_name(c->getPort("\\A").as_wire()->name)];
				SigSpec my_y = module->wires_[remap_name(c->getPort("\\Y").as_wire()->name)];
				module->connect(my_y, my_a);
				continue;
			}

			RTLIL::Cell *cell = module->addCell(remap_name(c->name), c->type);
			if (markgroups) cell->attributes["\\abcgroup"] = map_autoidx;
			cell->parameters = c->parameters;
			for (auto &conn : c->connections()) {
				RTLIL::SigSpec newsig;
				for (auto &c : conn.second.chunks()) {
					if (c.width == 0)
						continue;
					log_assert(c.width == 1);
					newsig.append(module->wires_[remap_name(c.wire->name)]);
				}
				cell->setPort(conn.first, newsig);
			}
			design->select(module, cell);
		}

		// FIXME: Better way to clean out module contents?
		module->connections_.clear();

		for (auto conn : mapped_mod->connections()) {
			if (!conn.first.is_fully_const()) {
				auto chunks = conn.first.chunks();
				for (auto &c : chunks)
					c.wire = module->wires_[remap_name(c.wire->name)];
				conn.first = std::move(chunks);
			}
			if (!conn.second.is_fully_const()) {
				auto chunks = conn.second.chunks();
				for (auto &c : chunks)
					c.wire = module->wires_[remap_name(c.wire->name)];
				conn.second = std::move(chunks);
			}
			module->connect(conn);
		}

		if (recover_init)
			for (auto wire : mapped_mod->wires()) {
				if (wire->attributes.count("\\init")) {
					Wire *w = module->wires_[remap_name(wire->name)];
					log_assert(w->attributes.count("\\init") == 0);
					w->attributes["\\init"] = wire->attributes.at("\\init");
				}
			}

		for (auto &it : cell_stats)
			log("ABC RESULTS:   %15s cells: %8d\n", it.first.c_str(), it.second);
		int in_wires = 0, out_wires = 0;
		//for (auto &si : signal_list)
		//	if (si.is_port) {
		//		char buffer[100];
		//		snprintf(buffer, 100, "\\n%d", si.id);
		//		RTLIL::SigSig conn;
		//		if (si.type != G(NONE)) {
		//			conn.first = si.bit;
		//			conn.second = RTLIL::SigSpec(module->wires_[remap_name(buffer)]);
		//			out_wires++;
		//		} else {
		//			conn.first = RTLIL::SigSpec(module->wires_[remap_name(buffer)]);
		//			conn.second = si.bit;
		//			in_wires++;
		//		}
		//		module->connect(conn);
		//	}

		for (auto &it : mapped_mod->wires_) {
			RTLIL::Wire *w = it.second;
			if (!w->port_input && !w->port_output)
				continue;
			RTLIL::Wire *wire = module->wire(remap_name(w->name));
			if (w->port_input) {
				RTLIL::SigSig conn;
				conn.first = wire;
				conn.second = module->wire(w->name);
				if (conn.second.empty())
					log_error("Input port %s not found in original module.\n", w->name.c_str());
				in_wires++;
				module->connect(conn);
			}
			else if (w->port_output) {
				RTLIL::SigSig conn;
				conn.first = module->wire(w->name);
				if (conn.first.empty())
					log_error("Output port %s not found in original module.\n", w->name.c_str());
				conn.second = wire;
				out_wires++;
				module->connect(conn);
			}
		}
		//log("ABC RESULTS:        internal signals: %8d\n", int(signal_list.size()) - in_wires - out_wires);
		log("ABC RESULTS:           input signals: %8d\n", in_wires);
		log("ABC RESULTS:          output signals: %8d\n", out_wires);

		delete mapped_design;
	}
	//else
	//{
	//	log("Don't call ABC as there is nothing to map.\n");
	//}

	if (cleanup)
	{
		log("Removing temp directory.\n");
		remove_directory(tempdir_name);
	}

	log_pop();
}

struct Abc9Pass : public Pass {
	Abc9Pass() : Pass("abc9", "use ABC for technology mapping") { }
	void help() YS_OVERRIDE
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    abc9 [options] [selection]\n");
		log("\n");
		log("This pass uses the ABC tool [1] for technology mapping of yosys's internal gate\n");
		log("library to a target architecture.\n");
		log("\n");
		log("    -exe <command>\n");
#ifdef ABCEXTERNAL
		log("        use the specified command instead of \"" ABCEXTERNAL "\" to execute ABC.\n");
#else
		log("        use the specified command instead of \"<yosys-bindir>/yosys-abc\" to execute ABC.\n");
#endif
		log("        This can e.g. be used to call a specific version of ABC or a wrapper.\n");
		log("\n");
		log("    -script <file>\n");
		log("        use the specified ABC script file instead of the default script.\n");
		log("\n");
		log("        if <file> starts with a plus sign (+), then the rest of the filename\n");
		log("        string is interpreted as the command string to be passed to ABC. The\n");
		log("        leading plus sign is removed and all commas (,) in the string are\n");
		log("        replaced with blanks before the string is passed to ABC.\n");
		log("\n");
		log("        if no -script parameter is given, the following scripts are used:\n");
		log("\n");
		log("        for -liberty without -constr:\n");
		log("%s\n", fold_abc_cmd(ABC_COMMAND_LIB).c_str());
		log("\n");
		log("        for -liberty with -constr:\n");
		log("%s\n", fold_abc_cmd(ABC_COMMAND_CTR).c_str());
		log("\n");
		log("        for -lut/-luts (only one LUT size):\n");
		log("%s\n", fold_abc_cmd(ABC_COMMAND_LUT "; lutpack {S}").c_str());
		log("\n");
		log("        for -lut/-luts (different LUT sizes):\n");
		log("%s\n", fold_abc_cmd(ABC_COMMAND_LUT).c_str());
		log("\n");
		log("        for -sop:\n");
		log("%s\n", fold_abc_cmd(ABC_COMMAND_SOP).c_str());
		log("\n");
		log("        otherwise:\n");
		log("%s\n", fold_abc_cmd(ABC_COMMAND_DFL).c_str());
		log("\n");
		log("    -fast\n");
		log("        use different default scripts that are slightly faster (at the cost\n");
		log("        of output quality):\n");
		log("\n");
		log("        for -liberty without -constr:\n");
		log("%s\n", fold_abc_cmd(ABC_FAST_COMMAND_LIB).c_str());
		log("\n");
		log("        for -liberty with -constr:\n");
		log("%s\n", fold_abc_cmd(ABC_FAST_COMMAND_CTR).c_str());
		log("\n");
		log("        for -lut/-luts:\n");
		log("%s\n", fold_abc_cmd(ABC_FAST_COMMAND_LUT).c_str());
		log("\n");
		log("        for -sop:\n");
		log("%s\n", fold_abc_cmd(ABC_FAST_COMMAND_SOP).c_str());
		log("\n");
		log("        otherwise:\n");
		log("%s\n", fold_abc_cmd(ABC_FAST_COMMAND_DFL).c_str());
		log("\n");
		log("    -liberty <file>\n");
		log("        generate netlists for the specified cell library (using the liberty\n");
		log("        file format).\n");
		log("\n");
		log("    -constr <file>\n");
		log("        pass this file with timing constraints to ABC. use with -liberty.\n");
		log("\n");
		log("        a constr file contains two lines:\n");
		log("            set_driving_cell <cell_name>\n");
		log("            set_load <floating_point_number>\n");
		log("\n");
		log("        the set_driving_cell statement defines which cell type is assumed to\n");
		log("        drive the primary inputs and the set_load statement sets the load in\n");
		log("        femtofarads for each primary output.\n");
		log("\n");
		log("    -D <picoseconds>\n");
		log("        set delay target. the string {D} in the default scripts above is\n");
		log("        replaced by this option when used, and an empty string otherwise.\n");
		log("        this also replaces 'dretime' with 'dretime; retime -o {D}' in the\n");
		log("        default scripts above.\n");
		log("\n");
		log("    -I <num>\n");
		log("        maximum number of SOP inputs.\n");
		log("        (replaces {I} in the default scripts above)\n");
		log("\n");
		log("    -P <num>\n");
		log("        maximum number of SOP products.\n");
		log("        (replaces {P} in the default scripts above)\n");
		log("\n");
		log("    -S <num>\n");
		log("        maximum number of LUT inputs shared.\n");
		log("        (replaces {S} in the default scripts above, default: -S 1)\n");
		log("\n");
		log("    -lut <width>\n");
		log("        generate netlist using luts of (max) the specified width.\n");
		log("\n");
		log("    -lut <w1>:<w2>\n");
		log("        generate netlist using luts of (max) the specified width <w2>. All\n");
		log("        luts with width <= <w1> have constant cost. for luts larger than <w1>\n");
		log("        the area cost doubles with each additional input bit. the delay cost\n");
		log("        is still constant for all lut widths.\n");
		log("\n");
		log("    -luts <cost1>,<cost2>,<cost3>,<sizeN>:<cost4-N>,..\n");
		log("        generate netlist using luts. Use the specified costs for luts with 1,\n");
		log("        2, 3, .. inputs.\n");
		log("\n");
		log("    -sop\n");
		log("        map to sum-of-product cells and inverters\n");
		log("\n");
		// log("    -mux4, -mux8, -mux16\n");
		// log("        try to extract 4-input, 8-input, and/or 16-input muxes\n");
		// log("        (ignored when used with -liberty or -lut)\n");
		// log("\n");
		log("    -g type1,type2,...\n");
		log("        Map to the specified list of gate types. Supported gates types are:\n");
		log("        AND, NAND, OR, NOR, XOR, XNOR, ANDNOT, ORNOT, MUX, AOI3, OAI3, AOI4, OAI4.\n");
		log("        (The NOT gate is always added to this list automatically.)\n");
		log("\n");
		log("        The following aliases can be used to reference common sets of gate types:\n");
		log("          simple: AND OR XOR MUX\n");
		log("          cmos2: NAND NOR\n");
		log("          cmos3: NAND NOR AOI3 OAI3\n");
		log("          cmos4: NAND NOR AOI3 OAI3 AOI4 OAI4\n");
		log("          gates: AND NAND OR NOR XOR XNOR ANDNOT ORNOT\n");
		log("          aig: AND NAND OR NOR ANDNOT ORNOT\n");
		log("\n");
		log("        Prefix a gate type with a '-' to remove it from the list. For example\n");
		log("        the arguments 'AND,OR,XOR' and 'simple,-MUX' are equivalent.\n");
		log("\n");
		log("    -dff\n");
		log("        also pass $_DFF_?_ and $_DFFE_??_ cells through ABC. modules with many\n");
		log("        clock domains are automatically partitioned in clock domains and each\n");
		log("        domain is passed through ABC independently.\n");
		log("\n");
		log("    -clk [!]<clock-signal-name>[,[!]<enable-signal-name>]\n");
		log("        use only the specified clock domain. this is like -dff, but only FF\n");
		log("        cells that belong to the specified clock domain are used.\n");
		log("\n");
		log("    -keepff\n");
		log("        set the \"keep\" attribute on flip-flop output wires. (and thus preserve\n");
		log("        them, for example for equivalence checking.)\n");
		log("\n");
		log("    -nocleanup\n");
		log("        when this option is used, the temporary files created by this pass\n");
		log("        are not removed. this is useful for debugging.\n");
		log("\n");
		log("    -showtmp\n");
		log("        print the temp dir name in log. usually this is suppressed so that the\n");
		log("        command output is identical across runs.\n");
		log("\n");
		log("    -markgroups\n");
		log("        set a 'abcgroup' attribute on all objects created by ABC. The value of\n");
		log("        this attribute is a unique integer for each ABC process started. This\n");
		log("        is useful for debugging the partitioning of clock domains.\n");
		log("\n");
		log("When neither -liberty nor -lut is used, the Yosys standard cell library is\n");
		log("loaded into ABC before the ABC script is executed.\n");
		log("\n");
		log("Note that this is a logic optimization pass within Yosys that is calling ABC\n");
		log("internally. This is not going to \"run ABC on your design\". It will instead run\n");
		log("ABC on logic snippets extracted from your design. You will not get any useful\n");
		log("output when passing an ABC script that writes a file. Instead write your full\n");
		log("design as BLIF file with write_blif and the load that into ABC externally if\n");
		log("you want to use ABC to convert your design into another format.\n");
		log("\n");
		log("[1] http://www.eecs.berkeley.edu/~alanmi/abc/\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) YS_OVERRIDE
	{
		log_header(design, "Executing ABC9 pass (technology mapping using ABC).\n");
		log_push();

		assign_map.clear();
		signal_map.clear();
		signal_init.clear();
		pi_map.clear();
		po_map.clear();

#ifdef ABCEXTERNAL
		std::string exe_file = ABCEXTERNAL;
#else
		std::string exe_file = proc_self_dirname() + "yosys-abc";
#endif
		std::string script_file, liberty_file, constr_file, clk_str;
		std::string delay_target, sop_inputs, sop_products, lutin_shared = "-S 1";
		bool fast_mode = false, dff_mode = false, keepff = false, cleanup = true;
		bool show_tempdir = false, sop_mode = false;
		show_tempdir = true; cleanup = false;
		vector<int> lut_costs;
		markgroups = false;

		map_mux4 = false;
		map_mux8 = false;
		map_mux16 = false;
		enabled_gates.clear();

#ifdef _WIN32
#ifndef ABCEXTERNAL
		if (!check_file_exists(exe_file + ".exe") && check_file_exists(proc_self_dirname() + "..\\yosys-abc.exe"))
			exe_file = proc_self_dirname() + "..\\yosys-abc";
#endif
#endif

		size_t argidx;
		char pwd [PATH_MAX];
		if (!getcwd(pwd, sizeof(pwd))) {
			log_cmd_error("getcwd failed: %s\n", strerror(errno));
			log_abort();
		}
		for (argidx = 1; argidx < args.size(); argidx++) {
			std::string arg = args[argidx];
			if (arg == "-exe" && argidx+1 < args.size()) {
				exe_file = args[++argidx];
				continue;
			}
			if (arg == "-script" && argidx+1 < args.size()) {
				script_file = args[++argidx];
				rewrite_filename(script_file);
				if (!script_file.empty() && !is_absolute_path(script_file) && script_file[0] != '+')
					script_file = std::string(pwd) + "/" + script_file;
				continue;
			}
			if (arg == "-liberty" && argidx+1 < args.size()) {
				liberty_file = args[++argidx];
				rewrite_filename(liberty_file);
				if (!liberty_file.empty() && !is_absolute_path(liberty_file))
					liberty_file = std::string(pwd) + "/" + liberty_file;
				continue;
			}
			if (arg == "-constr" && argidx+1 < args.size()) {
				rewrite_filename(constr_file);
				constr_file = args[++argidx];
				if (!constr_file.empty() && !is_absolute_path(constr_file))
					constr_file = std::string(pwd) + "/" + constr_file;
				continue;
			}
			if (arg == "-D" && argidx+1 < args.size()) {
				delay_target = "-D " + args[++argidx];
				continue;
			}
			if (arg == "-I" && argidx+1 < args.size()) {
				sop_inputs = "-I " + args[++argidx];
				continue;
			}
			if (arg == "-P" && argidx+1 < args.size()) {
				sop_products = "-P " + args[++argidx];
				continue;
			}
			if (arg == "-S" && argidx+1 < args.size()) {
				lutin_shared = "-S " + args[++argidx];
				continue;
			}
			if (arg == "-lut" && argidx+1 < args.size()) {
				string arg = args[++argidx];
				size_t pos = arg.find_first_of(':');
				int lut_mode = 0, lut_mode2 = 0;
				if (pos != string::npos) {
					lut_mode = atoi(arg.substr(0, pos).c_str());
					lut_mode2 = atoi(arg.substr(pos+1).c_str());
				} else {
					lut_mode = atoi(arg.c_str());
					lut_mode2 = lut_mode;
				}
				lut_costs.clear();
				for (int i = 0; i < lut_mode; i++)
					lut_costs.push_back(1);
				for (int i = lut_mode; i < lut_mode2; i++)
					lut_costs.push_back(2 << (i - lut_mode));
				continue;
			}
			if (arg == "-luts" && argidx+1 < args.size()) {
				lut_costs.clear();
				for (auto &tok : split_tokens(args[++argidx], ",")) {
					auto parts = split_tokens(tok, ":");
					if (GetSize(parts) == 0 && !lut_costs.empty())
						lut_costs.push_back(lut_costs.back());
					else if (GetSize(parts) == 1)
						lut_costs.push_back(atoi(parts.at(0).c_str()));
					else if (GetSize(parts) == 2)
						while (GetSize(lut_costs) < atoi(parts.at(0).c_str()))
							lut_costs.push_back(atoi(parts.at(1).c_str()));
					else
						log_cmd_error("Invalid -luts syntax.\n");
				}
				continue;
			}
			if (arg == "-sop") {
				sop_mode = true;
				continue;
			}
			if (arg == "-mux4") {
				map_mux4 = true;
				continue;
			}
			if (arg == "-mux8") {
				map_mux8 = true;
				continue;
			}
			if (arg == "-mux16") {
				map_mux16 = true;
				continue;
			}
			if (arg == "-g" && argidx+1 < args.size()) {
				for (auto g : split_tokens(args[++argidx], ",")) {
					vector<string> gate_list;
					bool remove_gates = false;
					if (GetSize(g) > 0 && g[0] == '-') {
						remove_gates = true;
						g = g.substr(1);
					}
					if (g == "AND") goto ok_gate;
					if (g == "NAND") goto ok_gate;
					if (g == "OR") goto ok_gate;
					if (g == "NOR") goto ok_gate;
					if (g == "XOR") goto ok_gate;
					if (g == "XNOR") goto ok_gate;
					if (g == "ANDNOT") goto ok_gate;
					if (g == "ORNOT") goto ok_gate;
					if (g == "MUX") goto ok_gate;
					if (g == "AOI3") goto ok_gate;
					if (g == "OAI3") goto ok_gate;
					if (g == "AOI4") goto ok_gate;
					if (g == "OAI4") goto ok_gate;
					if (g == "simple") {
						gate_list.push_back("AND");
						gate_list.push_back("OR");
						gate_list.push_back("XOR");
						gate_list.push_back("MUX");
						goto ok_alias;
					}
					if (g == "cmos2") {
						gate_list.push_back("NAND");
						gate_list.push_back("NOR");
						goto ok_alias;
					}
					if (g == "cmos3") {
						gate_list.push_back("NAND");
						gate_list.push_back("NOR");
						gate_list.push_back("AOI3");
						gate_list.push_back("OAI3");
						goto ok_alias;
					}
					if (g == "cmos4") {
						gate_list.push_back("NAND");
						gate_list.push_back("NOR");
						gate_list.push_back("AOI3");
						gate_list.push_back("OAI3");
						gate_list.push_back("AOI4");
						gate_list.push_back("OAI4");
						goto ok_alias;
					}
					if (g == "gates") {
						gate_list.push_back("AND");
						gate_list.push_back("NAND");
						gate_list.push_back("OR");
						gate_list.push_back("NOR");
						gate_list.push_back("XOR");
						gate_list.push_back("XNOR");
						gate_list.push_back("ANDNOT");
						gate_list.push_back("ORNOT");
						goto ok_alias;
					}
					if (g == "aig") {
						gate_list.push_back("AND");
						gate_list.push_back("NAND");
						gate_list.push_back("OR");
						gate_list.push_back("NOR");
						gate_list.push_back("ANDNOT");
						gate_list.push_back("ORNOT");
						goto ok_alias;
					}
					cmd_error(args, argidx, stringf("Unsupported gate type: %s", g.c_str()));
				ok_gate:
					gate_list.push_back(g);
				ok_alias:
					for (auto gate : gate_list) {
						if (remove_gates)
							enabled_gates.erase(gate);
						else
							enabled_gates.insert(gate);
					}
				}
				continue;
			}
			if (arg == "-fast") {
				fast_mode = true;
				continue;
			}
			if (arg == "-dff") {
				dff_mode = true;
				continue;
			}
			if (arg == "-clk" && argidx+1 < args.size()) {
				clk_str = args[++argidx];
				dff_mode = true;
				continue;
			}
			if (arg == "-keepff") {
				keepff = true;
				continue;
			}
			if (arg == "-nocleanup") {
				cleanup = false;
				continue;
			}
			if (arg == "-showtmp") {
				show_tempdir = true;
				continue;
			}
			if (arg == "-markgroups") {
				markgroups = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		if (!lut_costs.empty() && !liberty_file.empty())
			log_cmd_error("Got -lut and -liberty! This two options are exclusive.\n");
		if (!constr_file.empty() && liberty_file.empty())
			log_cmd_error("Got -constr but no -liberty!\n");

		for (auto mod : design->selected_modules())
		{
			if (mod->processes.size() > 0) {
				log("Skipping module %s as it contains processes.\n", log_id(mod));
				continue;
			}

			assign_map.set(mod);
			signal_init.clear();

			for (Wire *wire : mod->wires())
				if (wire->attributes.count("\\init")) {
					SigSpec initsig = assign_map(wire);
					Const initval = wire->attributes.at("\\init");
					for (int i = 0; i < GetSize(initsig) && i < GetSize(initval); i++)
						switch (initval[i]) {
							case State::S0:
								signal_init[initsig[i]] = State::S0;
								break;
							case State::S1:
								signal_init[initsig[i]] = State::S0;
								break;
							default:
								break;
						}
				}

			if (!dff_mode || !clk_str.empty()) {
				abc9_module(design, mod, script_file, exe_file, liberty_file, constr_file, cleanup, lut_costs, dff_mode, clk_str, keepff,
						delay_target, sop_inputs, sop_products, lutin_shared, fast_mode, mod->selected_cells(), show_tempdir, sop_mode);
				continue;
			}

			CellTypes ct(design);

			std::vector<RTLIL::Cell*> all_cells = mod->selected_cells();
			std::set<RTLIL::Cell*> unassigned_cells(all_cells.begin(), all_cells.end());

			std::set<RTLIL::Cell*> expand_queue, next_expand_queue;
			std::set<RTLIL::Cell*> expand_queue_up, next_expand_queue_up;
			std::set<RTLIL::Cell*> expand_queue_down, next_expand_queue_down;

			typedef tuple<bool, RTLIL::SigSpec, bool, RTLIL::SigSpec> clkdomain_t;
			std::map<clkdomain_t, std::vector<RTLIL::Cell*>> assigned_cells;
			std::map<RTLIL::Cell*, clkdomain_t> assigned_cells_reverse;

			std::map<RTLIL::Cell*, std::set<RTLIL::SigBit>> cell_to_bit, cell_to_bit_up, cell_to_bit_down;
			std::map<RTLIL::SigBit, std::set<RTLIL::Cell*>> bit_to_cell, bit_to_cell_up, bit_to_cell_down;

			for (auto cell : all_cells)
			{
				clkdomain_t key;

				for (auto &conn : cell->connections())
				for (auto bit : conn.second) {
					bit = assign_map(bit);
					if (bit.wire != nullptr) {
						cell_to_bit[cell].insert(bit);
						bit_to_cell[bit].insert(cell);
						if (ct.cell_input(cell->type, conn.first)) {
							cell_to_bit_up[cell].insert(bit);
							bit_to_cell_down[bit].insert(cell);
						}
						if (ct.cell_output(cell->type, conn.first)) {
							cell_to_bit_down[cell].insert(bit);
							bit_to_cell_up[bit].insert(cell);
						}
					}
				}

				if (cell->type == "$_DFF_N_" || cell->type == "$_DFF_P_")
				{
					key = clkdomain_t(cell->type == "$_DFF_P_", assign_map(cell->getPort("\\C")), true, RTLIL::SigSpec());
				}
				else
				if (cell->type == "$_DFFE_NN_" || cell->type == "$_DFFE_NP_" || cell->type == "$_DFFE_PN_" || cell->type == "$_DFFE_PP_")
				{
					bool this_clk_pol = cell->type == "$_DFFE_PN_" || cell->type == "$_DFFE_PP_";
					bool this_en_pol = cell->type == "$_DFFE_NP_" || cell->type == "$_DFFE_PP_";
					key = clkdomain_t(this_clk_pol, assign_map(cell->getPort("\\C")), this_en_pol, assign_map(cell->getPort("\\E")));
				}
				else
					continue;

				unassigned_cells.erase(cell);
				expand_queue.insert(cell);
				expand_queue_up.insert(cell);
				expand_queue_down.insert(cell);

				assigned_cells[key].push_back(cell);
				assigned_cells_reverse[cell] = key;
			}

			while (!expand_queue_up.empty() || !expand_queue_down.empty())
			{
				if (!expand_queue_up.empty())
				{
					RTLIL::Cell *cell = *expand_queue_up.begin();
					clkdomain_t key = assigned_cells_reverse.at(cell);
					expand_queue_up.erase(cell);

					for (auto bit : cell_to_bit_up[cell])
					for (auto c : bit_to_cell_up[bit])
						if (unassigned_cells.count(c)) {
							unassigned_cells.erase(c);
							next_expand_queue_up.insert(c);
							assigned_cells[key].push_back(c);
							assigned_cells_reverse[c] = key;
							expand_queue.insert(c);
						}
				}

				if (!expand_queue_down.empty())
				{
					RTLIL::Cell *cell = *expand_queue_down.begin();
					clkdomain_t key = assigned_cells_reverse.at(cell);
					expand_queue_down.erase(cell);

					for (auto bit : cell_to_bit_down[cell])
					for (auto c : bit_to_cell_down[bit])
						if (unassigned_cells.count(c)) {
							unassigned_cells.erase(c);
							next_expand_queue_up.insert(c);
							assigned_cells[key].push_back(c);
							assigned_cells_reverse[c] = key;
							expand_queue.insert(c);
						}
				}

				if (expand_queue_up.empty() && expand_queue_down.empty()) {
					expand_queue_up.swap(next_expand_queue_up);
					expand_queue_down.swap(next_expand_queue_down);
				}
			}

			while (!expand_queue.empty())
			{
				RTLIL::Cell *cell = *expand_queue.begin();
				clkdomain_t key = assigned_cells_reverse.at(cell);
				expand_queue.erase(cell);

				for (auto bit : cell_to_bit.at(cell)) {
					for (auto c : bit_to_cell[bit])
						if (unassigned_cells.count(c)) {
							unassigned_cells.erase(c);
							next_expand_queue.insert(c);
							assigned_cells[key].push_back(c);
							assigned_cells_reverse[c] = key;
						}
					bit_to_cell[bit].clear();
				}

				if (expand_queue.empty())
					expand_queue.swap(next_expand_queue);
			}

			clkdomain_t key(true, RTLIL::SigSpec(), true, RTLIL::SigSpec());
			for (auto cell : unassigned_cells) {
				assigned_cells[key].push_back(cell);
				assigned_cells_reverse[cell] = key;
			}

			log_header(design, "Summary of detected clock domains:\n");
			for (auto &it : assigned_cells)
				log("  %d cells in clk=%s%s, en=%s%s\n", GetSize(it.second),
						std::get<0>(it.first) ? "" : "!", log_signal(std::get<1>(it.first)),
						std::get<2>(it.first) ? "" : "!", log_signal(std::get<3>(it.first)));

			for (auto &it : assigned_cells) {
				clk_polarity = std::get<0>(it.first);
				clk_sig = assign_map(std::get<1>(it.first));
				en_polarity = std::get<2>(it.first);
				en_sig = assign_map(std::get<3>(it.first));
				abc9_module(design, mod, script_file, exe_file, liberty_file, constr_file, cleanup, lut_costs, !clk_sig.empty(), "$",
						keepff, delay_target, sop_inputs, sop_products, lutin_shared, fast_mode, it.second, show_tempdir, sop_mode);
				assign_map.set(mod);
			}
		}

		assign_map.clear();
		signal_map.clear();
		signal_init.clear();
		pi_map.clear();
		po_map.clear();

		log_pop();
	}
} Abc9Pass;

PRIVATE_NAMESPACE_END