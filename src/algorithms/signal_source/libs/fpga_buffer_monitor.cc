/*!
 * \file fpga_buffer_monitor.cc
 * \brief Check receiver buffer overflow and monitor the status of the receiver buffers.
 * \authors <ul>
 *    <li> Marc Majoral, 2020. mmajoral(at)cttc.es
 * </ul>
 *
 * Class that checks the receiver buffer overflow flags and monitors the status of the receiver buffers.
 *
 *
 * -----------------------------------------------------------------------------
 *
 * GNSS-SDR is a Global Navigation Satellite System software-defined receiver.
 * This file is part of GNSS-SDR.
 *
 * Copyright (C) 2010-2020  (see AUTHORS file for a list of contributors)
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -----------------------------------------------------------------------------
 */

#include "fpga_buffer_monitor.h"
#include "command_event.h"
#include "gnss_sdr_create_directory.h"
#include "gnss_sdr_filesystem.h"
#include <glog/logging.h>
#include <ctime>       // for time, localtime
#include <fcntl.h>     // for open, O_RDWR, O_SYNC
#include <fstream>     // for string, ofstream
#include <iostream>    // for cout
#include <sys/mman.h>  // for mmap

Fpga_buffer_monitor::Fpga_buffer_monitor(const std::string &device_name, uint32_t num_freq_bands, bool dump, std::string dump_filename, Concurrent_Queue<pmt::pmt_t> *queue)
{
    d_num_freq_bands = num_freq_bands;
    d_dump = dump;
    d_dump_filename = dump_filename;
    d_queue = queue;

    // open device descriptor
    if ((d_device_descriptor = open(device_name.c_str(), O_RDWR | O_SYNC)) == -1)
        {
            LOG(WARNING) << "Cannot open deviceio" << device_name;
        }

    // device memory map
    d_map_base = reinterpret_cast<volatile unsigned *>(mmap(nullptr, FPGA_PAGE_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, d_device_descriptor, 0));

    if (d_map_base == reinterpret_cast<void *>(-1))
        {
            LOG(WARNING) << "Cannot map the FPGA buffer monitor module";
            std::cout << "Could not map the FPGA buffer monitor \n";
        }

    // sanity check: check test register
    if (buffer_monitor_test_register() < 0)
        {
            LOG(WARNING) << "FPGA buffer monitor test register sanity check failed";
            std::cout << "FPGA buffer monitor test register sanity check failed\n";
        }
    else
        {
            LOG(INFO) << "FPGA buffer monitor test register sanity check success !";
        }

    DLOG(INFO) << "FPGA buffer monitor class created";

    // initialize maximum buffer occupancy in case buffer monitoring is enabled
    d_max_buff_occ_freq_band_0 = 0;
    d_max_buff_occ_freq_band_1 = 0;

    if (d_dump)
        {
            std::string dump_path;
            // Get path
            if (d_dump_filename.find_last_of('/') != std::string::npos)
                {
                    const std::string dump_filename_ = d_dump_filename.substr(d_dump_filename.find_last_of('/') + 1);
                    dump_path = d_dump_filename.substr(0, d_dump_filename.find_last_of('/'));
                    d_dump_filename = dump_filename_;
                }
            else
                {
                    dump_path = std::string(".");
                }
            if (d_dump_filename.empty())
                {
                    d_dump_filename = "FPGA_buffer_monitor";
                }
            // remove extension if any
            if (d_dump_filename.substr(1).find_last_of('.') != std::string::npos)
                {
                    d_dump_filename = d_dump_filename.substr(0, d_dump_filename.find_last_of('.'));
                }
            d_dump_filename = dump_path + fs::path::preferred_separator + d_dump_filename;
            // create directory
            if (!gnss_sdr_create_directory(dump_path))
                {
                    std::cerr << "GNSS-SDR cannot create dump file for the Buffer Monitor block. Wrong permissions?\n";
                    d_dump = false;
                }

            std::string dump_filename_ = d_dump_filename;
            dump_filename_.append(".dat");

            if (!d_dump_file.is_open())
                {
                    try
                        {
                            d_dump_file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
                            d_dump_file.open(dump_filename_.c_str(), std::ios::out | std::ios::binary);
                            LOG(INFO) << "FPGA buffer monitor dump enabled. Log file: " << dump_filename_.c_str();
                        }
                    catch (const std::ifstream::failure &e)
                        {
                            LOG(WARNING) << "Exception opening FPGA buffer monitor dump file " << e.what();
                        }
                }
        }
}


Fpga_buffer_monitor::~Fpga_buffer_monitor()
{
    close_device();

    if (d_dump)
        {
            if (d_dump_file.is_open())
                {
                    try
                        {
                            d_dump_file.close();
                        }
                    catch (const std::exception &ex)
                        {
                            LOG(WARNING) << "Exception in FPGA buffer monitor destructor: " << ex.what();
                        }
                }
        }
}


void Fpga_buffer_monitor::check_buffer_overflow_and_monitor_buffer_status(void)
{
    // check buffer overflow flags
    bool overflow_detected = false;
    uint32_t buffer_overflow_status = d_map_base[overflow_flags_reg_addr];

    if ((buffer_overflow_status & overflow_freq_band_0_bit_pos) != 0)
        {
            overflow_detected = true;
            std::cout << "Buffer overflow in frequency band 0" << std::endl;
        }

    if (d_num_freq_bands > 1)
        {
            if ((buffer_overflow_status & overflow_freq_band_1_bit_pos) != 0)
                {
                    overflow_detected = true;
                    std::cout << "Buffer overflow in frequency band 1" << std::endl;
                }
        }

    if (overflow_detected)
        {
            LOG(INFO) << "Stopping receiver, FPGA buffer overflow detected.";
            d_queue->push(pmt::make_any(command_event_make(200, 0)));
        }

    // buffer monitor
    if (d_dump == 1)
        {
            uint32_t current_buff_occ_freq_band_0 = d_map_base[current_buff_occ_freq_band_0_reg_addr] * num_sapmples_per_buffer_element;
            uint32_t temp_max_buff_occ_freq_band_0 = d_map_base[max_buff_occ_freq_band_0_reg_addr] * num_sapmples_per_buffer_element;
            if (temp_max_buff_occ_freq_band_0 > d_max_buff_occ_freq_band_0)
                {
                    d_max_buff_occ_freq_band_0 = temp_max_buff_occ_freq_band_0;
                }

            time_t rawtime;
            struct tm *timeinfo;
            char buff_time_ch[80];

            time(&rawtime);
            timeinfo = localtime(&rawtime);

            strftime(buff_time_ch, sizeof(buff_time_ch), "%d-%m-%Y %H:%M:%S", timeinfo);
            std::string buffer_time(buff_time_ch);
            d_dump_file << buffer_time << " ";

            std::string buffer_txt;
            // current buffer occupancy frequency band 0
            buffer_txt = std::to_string(current_buff_occ_freq_band_0);
            d_dump_file << buffer_txt << " ";
            // temporary maximum buffer occupancy frequency band 0
            buffer_txt = std::to_string(temp_max_buff_occ_freq_band_0);
            d_dump_file << buffer_txt << " ";
            // maximum buffer occupancy frequency band 0
            buffer_txt = std::to_string(d_max_buff_occ_freq_band_0);
            d_dump_file << buffer_txt;

            if (d_num_freq_bands > 1)
                {
                    d_dump_file << " ";
                    uint32_t current_buff_occ_freq_band_1 = d_map_base[current_buff_occ_freq_band_1_reg_addr] * num_sapmples_per_buffer_element;
                    uint32_t temp_max_buff_occ_freq_band_1 = d_map_base[max_buff_occ_freq_band_1_reg_addr] * num_sapmples_per_buffer_element;
                    if (temp_max_buff_occ_freq_band_1 > d_max_buff_occ_freq_band_1)
                        {
                            d_max_buff_occ_freq_band_1 = temp_max_buff_occ_freq_band_1;
                        }

                    // current buffer occupancy frequency band 1
                    buffer_txt = std::to_string(current_buff_occ_freq_band_1);
                    d_dump_file << buffer_txt << " ";
                    // temporary maximum buffer occupancy frequency band 1
                    buffer_txt = std::to_string(temp_max_buff_occ_freq_band_1);
                    d_dump_file << buffer_txt << " ";
                    // maximum buffer occupancy frequency band 1
                    buffer_txt = std::to_string(d_max_buff_occ_freq_band_1);
                    d_dump_file << buffer_txt << std::endl;
                }
            else
                {
                    d_dump_file << std::endl;
                }
        }
}


int32_t Fpga_buffer_monitor::buffer_monitor_test_register(void)
{
    // write value to test register
    d_map_base[test_reg_addr] = test_register_writeval;
    // read value from test register
    uint32_t readval = d_map_base[test_reg_addr];

    if (test_register_writeval != readval)
        {
            return -1;
        }

    return 0;
}


void Fpga_buffer_monitor::close_device()
{
    auto *aux = const_cast<unsigned *>(d_map_base);
    if (munmap(static_cast<void *>(aux), FPGA_PAGE_SIZE) == -1)
        {
            std::cout << "Failed to unmap memory uio\n";
        }

    close(d_device_descriptor);
}
