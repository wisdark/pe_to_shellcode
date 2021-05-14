#include <windows.h>
#include <iostream>

#include "peconv.h"
#include "resource.h"

#define VERSION "0.9"

bool overwrite_hdr(BYTE *my_exe, size_t exe_size, DWORD raw)
{
	BYTE redir_code[] = "\x4D" //dec ebp
		"\x5A" //pop edx
		"\x45" //inc ebp
		"\x52" //push edx
		"\xE8\x00\x00\x00\x00" //call <next_line>
		"\x5B" // pop ebx
		"\x48\x83\xEB\x09" // sub ebx,9
		"\x53" // push ebx (Image Base)
		"\x48\x81\xC3" // add ebx,
		"\x59\x04\x00\x00" // value
		"\xFF\xD3" // call ebx
		"\xc3"; // ret

	size_t offset = sizeof(redir_code) - 8;

	memcpy(redir_code + offset, &raw, sizeof(DWORD));
	memcpy(my_exe, redir_code, sizeof(redir_code));
	return true;
}

BYTE* shellcodify(BYTE *my_exe, size_t exe_size, size_t &out_size, bool is64b)
{
	out_size = 0;
	size_t stub_size = 0;
	int res_id = is64b ? STUB64 : STUB32;
	BYTE *stub = peconv::load_resource_data(stub_size, res_id);
	if (!stub) {
		std::cout << "[-] Stub not loaded" << std::endl;
		return nullptr;
	}
	size_t ext_size = exe_size + stub_size;
	BYTE *ext_buf = peconv::alloc_aligned(ext_size, PAGE_READWRITE);
	if (!ext_buf) {
		return nullptr;
	}
	memcpy(ext_buf, my_exe, exe_size);
	memcpy(ext_buf + exe_size, stub, stub_size);

	DWORD raw_addr = exe_size;
	overwrite_hdr(ext_buf, ext_size, raw_addr);

	out_size = ext_size;
	return ext_buf;
}

template <typename IMAGE_TLS_DIRECTORY>
bool has_tls_callbacks(BYTE *my_exe, size_t exe_size)
{
	IMAGE_DATA_DIRECTORY* tls_dir = peconv::get_directory_entry(my_exe, IMAGE_DIRECTORY_ENTRY_TLS);
	if (!tls_dir) return false;

	IMAGE_TLS_DIRECTORY* tls = peconv::get_type_directory<IMAGE_TLS_DIRECTORY>((HMODULE)my_exe, IMAGE_DIRECTORY_ENTRY_TLS);
	if (!tls) return false;

	ULONGLONG base = peconv::get_image_base(my_exe);
	ULONGLONG callback_rva = tls->AddressOfCallBacks;
	if (callback_rva > base) {
		callback_rva -= base;
	}
	if (!peconv::validate_ptr(my_exe, exe_size, my_exe + callback_rva, sizeof(ULONGLONG))) {
		return false;
	}
	ULONGLONG *callback_addr = (ULONGLONG *)(my_exe + callback_rva);
	if (callback_addr == 0) {
		return false;
	}
	if (*callback_addr == 0) {
		return false;
	}
	return true;
}

bool is_supported_pe(BYTE *my_exe, size_t exe_size)
{
	if (!my_exe) return false;
	if (!peconv::has_relocations(my_exe)) {
		std::cout << "[-] The PE must have relocations!" << std::endl;
		return false;
	}
	if (peconv::get_subsystem(my_exe) != IMAGE_SUBSYSTEM_WINDOWS_GUI) {
		std::cout << "[WARNING] This is a console application! The recommended subsystem is GUI." << std::endl;
	}
	IMAGE_DATA_DIRECTORY* dotnet_dir = peconv::get_directory_entry(my_exe, IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR);
	if (dotnet_dir) {
		std::cout << "[-] .NET applications are not supported!" << std::endl;
		return false;
	}
	IMAGE_DATA_DIRECTORY* tls_dir = peconv::get_directory_entry(my_exe, IMAGE_DIRECTORY_ENTRY_TLS);
	if (tls_dir) {
		bool has_callback = false;
		if (!peconv::is64bit(my_exe)) {
			if (has_tls_callbacks<IMAGE_TLS_DIRECTORY32>(my_exe, exe_size)) {
				has_callback = true;
			}
		}
		else {
			if (has_tls_callbacks<IMAGE_TLS_DIRECTORY64>(my_exe, exe_size)) {
				has_callback = true;
			}
		}
		if (has_callback) {
			std::cout << "[WARNING] This application has TLS callbacks, which are not supported!" << std::endl;
		}
	}
	return true;
}

bool is_supported_pe(const std::string &in_path)
{
	std::cout << "Reading module from: " << in_path << std::endl;
	size_t exe_size = 0;
	BYTE *my_exe = peconv::load_pe_module(in_path.c_str(), exe_size, false, false);
	if (!my_exe) {
		std::cout << "[-] Could not read the input file!" << std::endl;
		return false;
	}

	bool is_ok = is_supported_pe(my_exe, exe_size);
	peconv::free_pe_buffer(my_exe);

	if (!is_ok) {
		std::cout << "[-] Not supported input file!" << std::endl;
		return false;
	}
	return true;
}

std::string make_out_name(std::string input_file)
{
	size_t found_indx = input_file.find_last_of(".");
	std::string ext = input_file.substr(found_indx + 1);
	std::string name = input_file.substr(0, found_indx);
	return name + ".shc." + ext;
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		std::cout << "~ pe2shc v." << VERSION << " ~\n"
			<< "Converts PE into shellcode.\nFor 32 & 64 bit PEs.\n";
		std::cout << "Args: <input_file> [output_file]" << std::endl;
		system("pause");
		return 0;
	}

	std::string in_path = argv[1];
	std::string  out_str = make_out_name(in_path);
	if (argc > 2) {
		out_str = argv[2];
	}

	if (!is_supported_pe(in_path)) {
		return -2;
	}

	size_t exe_size = 0;
	BYTE *my_exe = peconv::load_pe_module(in_path.c_str(), exe_size, false, false);
	if (!my_exe) {
		std::cout << "[-] Could not read the input file!" << std::endl;
		return -1;
	}

	bool is64b = peconv::is64bit(my_exe);
	size_t ext_size = 0;
	BYTE *ext_buf = shellcodify(my_exe, exe_size, ext_size, is64b);
	if (!ext_buf) {
		std::cout << "[-] Adding the stub failed!" << std::endl;
		peconv::free_file(my_exe);
		return -3;
	}
	// remap pe to raw == virtual, so that remapping on load will not be required
	peconv::t_pe_dump_mode dump_mode = peconv::PE_DUMP_REALIGN;
	ULONGLONG current_base = peconv::get_image_base(ext_buf);
	if (peconv::dump_pe(out_str.c_str(), ext_buf, ext_size, current_base, dump_mode)) {
		std::cout << "[+] Saved as: " << out_str << std::endl;
	}
	else {
		std::cout << "[-] Failed to save the output!" << std::endl;
	}
	peconv::free_file(my_exe);
	peconv::free_aligned(ext_buf);
	return 0;
}
