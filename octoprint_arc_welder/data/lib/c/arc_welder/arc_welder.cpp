////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Arc Welder: Anti-Stutter Library
//
// Compresses many G0/G1 commands into G2/G3(arc) commands where possible, ensuring the tool paths stay within the specified resolution.
// This reduces file size and the number of gcodes per second.
//
// Uses the 'Gcode Processor Library' for gcode parsing, position processing, logging, and other various functionality.
//
// Copyright(C) 2020 - Brad Hochgesang
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// This program is free software : you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU Affero General Public License for more details.
//
//
// You can contact the author at the following email address: 
// FormerLurker@pm.me
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "arc_welder.h"
#include <time.h>
#include <vector>
#include <sstream>
#include "utilities.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
arc_welder::arc_welder(std::string source_path, std::string target_path, logger * log, double resolution_mm, gcode_position_args args) : current_arc_(gcode_position_args_.position_buffer_size - 5, resolution_mm)
{
	p_logger_ = log;
	debug_logging_enabled_ = false;
	info_logging_enabled_ = false;
	error_logging_enabled_ = false;
	verbose_logging_enabled_ = false;

	logger_type_ = 0;
	progress_callback_ = NULL;
	verbose_output_ = false;
	absolute_e_offset_total_ = 0;
	source_path_ = source_path;
	target_path_ = target_path;
	resolution_mm_ = resolution_mm;
	gcode_position_args_ = args;
	notification_period_seconds = 1;
	lines_processed_ = 0;
	gcodes_processed_ = 0;
	file_size_ = 0;
	last_gcode_line_written_ = 0;
	points_compressed_ = 0;
	arcs_created_ = 0;
	waiting_for_line_ = false;
	waiting_for_arc_ = false;
	absolute_e_offset_ = 0;
	gcode_position_args_.set_num_extruders(8);
	for (int index = 0; index < 8; index++)
	{
		gcode_position_args_.retraction_lengths[0] = .0001;
		gcode_position_args_.z_lift_heights[0] = 0.001;
		gcode_position_args_.x_firmware_offsets[0] = 0.0;
		gcode_position_args_.y_firmware_offsets[0] = 0.0;
	}

	// We don't care about the printer settings, except for g91 influences extruder.
	p_source_position_ = new gcode_position(gcode_position_args_); 
	
	// Create a list of commands that will need rewritten absolute e values
	std::vector<std::string> absolute_e_rewrite_command_names;
	absolute_e_rewrite_command_names.push_back("G0");
	absolute_e_rewrite_command_names.push_back("G1");
	absolute_e_rewrite_command_names.push_back("G2");
	absolute_e_rewrite_command_names.push_back("G3");
	//absolute_e_rewrite_command_names.push_back("G92");
	
	for (unsigned int index = 0; index < absolute_e_rewrite_command_names.size(); index++)
	{
		absolute_e_rewrite_commands_.insert(absolute_e_rewrite_command_names[index]);
	}
}

arc_welder::arc_welder(std::string source_path, std::string target_path, logger* log, double resolution_mm, bool g90_g91_influences_extruder, int buffer_size)
	: arc_welder(source_path, target_path, log, resolution_mm, arc_welder::get_args_(g90_g91_influences_extruder, buffer_size))
{
	
}

arc_welder::arc_welder(std::string source_path, std::string target_path, logger * log, double resolution_mm, bool g90_g91_influences_extruder, int buffer_size, progress_callback callback)
	: arc_welder(source_path, target_path, log, resolution_mm, arc_welder::get_args_(g90_g91_influences_extruder, buffer_size))
{
	progress_callback_ = callback;
}

gcode_position_args arc_welder::get_args_(bool g90_g91_influences_extruder, int buffer_size)
{
	gcode_position_args args;
	// Configure gcode_position_args
	args.g90_influences_extruder = g90_g91_influences_extruder;
	args.position_buffer_size = buffer_size;
	args.autodetect_position = true;
	args.home_x = 0;
	args.home_x_none = true;
	args.home_y = 0;
	args.home_y_none = true;
	args.home_z = 0;
	args.home_z_none = true;
	args.shared_extruder = true;
	args.zero_based_extruder = true;


	args.default_extruder = 0;
	args.xyz_axis_default_mode = "absolute";
	args.e_axis_default_mode = "absolute";
	args.units_default = "millimeters";
	args.location_detection_commands = std::vector<std::string>();
	args.is_bound_ = false;
	args.is_circular_bed = false;
	args.x_min = -9999;
	args.x_max = 9999;
	args.y_min = -9999;
	args.y_max = 9999;
	args.z_min = -9999;
	args.z_max = 9999;
	return args;
}

arc_welder::~arc_welder()
{
	delete p_source_position_;
}

void arc_welder::set_logger_type(int logger_type)
{
	logger_type_ = logger_type;
}

void arc_welder::reset()
{
	lines_processed_ = 0;
	gcodes_processed_ = 0;
	last_gcode_line_written_ = 0;
	file_size_ = 0;
	points_compressed_ = 0;
	arcs_created_ = 0;
	waiting_for_line_ = false;
	waiting_for_arc_ = false;
	absolute_e_offset_ = 0;
}

long arc_welder::get_file_size(const std::string& file_path)
{
	// Todo:  Fix this function.  This is a pretty weak implementation :(
	std::ifstream file(file_path.c_str(), std::ios::in | std::ios::binary);
	const long l = (long)file.tellg();
	file.seekg(0, std::ios::end);
	const long m = (long)file.tellg();
	file.close();
	return (m - l);
}

double arc_welder::get_next_update_time() const
{
	return clock() + (notification_period_seconds * CLOCKS_PER_SEC);
}

double arc_welder::get_time_elapsed(double start_clock, double end_clock)
{
	return static_cast<double>(end_clock - start_clock) / CLOCKS_PER_SEC;
}

void arc_welder::process()
{
	verbose_logging_enabled_ = p_logger_->is_log_level_enabled(logger_type_, VERBOSE);
	debug_logging_enabled_ = p_logger_->is_log_level_enabled(logger_type_, DEBUG);
	info_logging_enabled_ = p_logger_->is_log_level_enabled(logger_type_, INFO);
	error_logging_enabled_ = p_logger_->is_log_level_enabled(logger_type_, ERROR);
	// reset tracking variables
	reset();
	// local variable to hold the progress update return.  If it's false, we will exit.
	bool continue_processing = true;
	
	// Create a stringstream we can use for messaging.
	std::stringstream stream;
	
	int read_lines_before_clock_check = 5000;
	double next_update_time = get_next_update_time();
	const clock_t start_clock = clock();
	file_size_ = get_file_size(source_path_);
	// Create the source file read stream and target write stream
	std::ifstream gcodeFile;
	gcodeFile.open(source_path_.c_str());
	output_file_.open(target_path_.c_str());
	std::string line;
	int lines_with_no_commands = 0;
	gcodeFile.sync_with_stdio(false);
	output_file_.sync_with_stdio(false);
	if (gcodeFile.is_open())
	{
		if (output_file_.is_open())
		{
			if (info_logging_enabled_)
			{
				stream.clear();
				stream.str("");
				stream << "Opened file for reading.  File Size: " << file_size_;
				p_logger_->log(logger_type_, DEBUG, stream.str());
			}
			parsed_command cmd;
			// Communicate every second
			while (std::getline(gcodeFile, line) && continue_processing)
			{
				lines_processed_++;

				cmd.clear();
				parser_.try_parse_gcode(line.c_str(), cmd);
				bool has_gcode = false;
				if (cmd.gcode.length() > 0)
				{
					has_gcode = true;
					gcodes_processed_++;
				}
				else
				{
					lines_with_no_commands++;
				}

				// Always process the command through the printer, even if no command is found
				// This is important so that comments can be analyzed
				//std::cout << "stabilization::process_file - updating position...";
				process_gcode(cmd, false);

				// Only continue to process if we've found a command.
				if (has_gcode)
				{
					if ((lines_processed_ % read_lines_before_clock_check) == 0 && next_update_time < clock())
					{
						long file_position = static_cast<long>(gcodeFile.tellg());
						// ToDo: tellg does not do what I think it does, but why?
						long bytesRemaining = file_size_ - file_position;
						double percentProgress = static_cast<double>(file_position) / static_cast<double>(file_size_) * 100.0;
						double secondsElapsed = get_time_elapsed(start_clock, clock());
						double bytesPerSecond = static_cast<double>(file_position) / secondsElapsed;
						double secondsToComplete = bytesRemaining / bytesPerSecond;
						continue_processing = on_progress_(percentProgress, secondsElapsed, secondsToComplete, gcodes_processed_, lines_processed_, points_compressed_, arcs_created_);
						next_update_time = get_next_update_time();
					}
				}
			}

			if (current_arc_.is_shape() && waiting_for_arc_)
			{
				process_gcode(cmd, true);
			}
			write_unwritten_gcodes_to_file();

			output_file_.close();
		}
		else
		{
			p_logger_->log_exception(logger_type_, "Unable to open the output file for writing.");
		}
		gcodeFile.close();
	}
	else
	{
		p_logger_->log_exception(logger_type_, "Unable to open the gcode file for processing.");
	}

	const clock_t end_clock = clock();
	const double total_seconds = static_cast<double>(end_clock - start_clock) / CLOCKS_PER_SEC;
	on_progress_(100, total_seconds, 0, gcodes_processed_, lines_processed_, points_compressed_, arcs_created_);
}

bool arc_welder::on_progress_(double percentComplete, double seconds_elapsed, double estimatedSecondsRemaining, int gcodesProcessed, int linesProcessed, int points_compressed, int arcs_created)
{
	if (progress_callback_ != NULL)
	{
		return progress_callback_(percentComplete, seconds_elapsed, estimatedSecondsRemaining, gcodesProcessed, linesProcessed, points_compressed, arcs_created);
	}
	std::stringstream stream;
	if (debug_logging_enabled_)
	{
		stream << percentComplete << "% complete in " << seconds_elapsed << " seconds with " << estimatedSecondsRemaining << " seconds remaining.  Gcodes Processed:" << gcodesProcessed << ", Current Line:" << linesProcessed << ", Points Compressed:" << points_compressed << ", ArcsCreated:" << arcs_created;
		p_logger_->log(logger_type_, DEBUG, stream.str());
	}

	return true;
}

int arc_welder::process_gcode(parsed_command cmd, bool is_end)
{
	// Update the position for the source gcode file
	p_source_position_->update(cmd, lines_processed_, gcodes_processed_, -1);

	position* p_cur_pos = p_source_position_->get_current_position_ptr();
	position* p_pre_pos = p_source_position_->get_previous_position_ptr();
	//std::cout << lines_processed_ << " - " << cmd.gcode << ", CurrentEAbsolute: " << cur_extruder.e <<", ExtrusionLength: " << cur_extruder.extrusion_length << ", Retraction Length: " << cur_extruder.retraction_length << ", IsExtruding: " << cur_extruder.is_extruding << ", IsRetracting: " << cur_extruder.is_retracting << ".\n";

	int lines_written = 0;
	// see if this point is an extrusion
	
	bool arc_added = false;
	bool clear_shapes = false;
	
	extruder extruder_current = p_cur_pos->get_current_extruder();
	point p(p_cur_pos->x, p_cur_pos->y, p_cur_pos->z, extruder_current.e_relative);

	// We need to make sure the printer is using absolute xyz, is extruding, and the extruder axis mode is the same as that of the previous position
	// TODO: Handle relative XYZ axis.  This is possible, but maybe not so important.
	if (
		!is_end && cmd.is_known_command && !cmd.is_empty && (
			(cmd.command == "G0" || cmd.command == "G1") &&
			utilities::is_equal(p_cur_pos->z, p_pre_pos->z) &&
			!p_cur_pos->is_relative &&
			(
				!waiting_for_arc_ ||
				(p_pre_pos->get_current_extruder().is_extruding && extruder_current.is_extruding) ||
				(p_pre_pos->get_current_extruder().is_retracting && extruder_current.is_retracting)
			) &&
			p_cur_pos->is_extruder_relative == p_pre_pos->is_extruder_relative &&
			(!waiting_for_arc_ || p_pre_pos->f == p_cur_pos->f) &&
			(!waiting_for_arc_ || p_pre_pos->feature_type_tag == p_cur_pos->feature_type_tag)
			)
	) {
		
		if (!waiting_for_arc_)
		{
			if (debug_logging_enabled_)
			{
				p_logger_->log(logger_type_, DEBUG, "Starting new arc from Gcode:" + cmd.gcode);
			}
			write_unwritten_gcodes_to_file();
			// add the previous point as the starting point for the current arc
			point previous_p(p_pre_pos->x, p_pre_pos->y, p_pre_pos->z, p_pre_pos->get_current_extruder().e_relative);
			// Don't add any extrusion, or you will over extrude!
			//std::cout << "Trying to add first point (" << p.x << "," << p.y << "," << p.z << ")...";
			current_arc_.try_add_point(previous_p, 0);
		}
		
		double e_relative = p_cur_pos->get_current_extruder().e_relative;
		int num_points = current_arc_.get_num_segments();
		arc_added = current_arc_.try_add_point(p, e_relative);
		if (arc_added)
		{
			if (!waiting_for_arc_)
			{
				waiting_for_arc_ = true;
			}
			else
			{
				if (debug_logging_enabled_)
				{
					if (num_points+1 == current_arc_.get_num_segments())
					{
						p_logger_->log(logger_type_, DEBUG, "Adding point to arc from Gcode:" + cmd.gcode);
					}
					{
						p_logger_->log(logger_type_, DEBUG, "Removed start point from arc and added a new point from Gcode:" + cmd.gcode);
					}
				}
			}
		}
	}
	else if (debug_logging_enabled_ ){
		if (is_end)
		{
			p_logger_->log(logger_type_, DEBUG, "Procesing final shape, if one exists.");
		}
		else if (!cmd.is_empty)
		{
			if (!cmd.is_known_command)
			{
				p_logger_->log(logger_type_, DEBUG, "Command '" + cmd.command + "' is Unknown.  Gcode:" + cmd.gcode);
			}
			else if (cmd.command != "G0" && cmd.command != "G1")
			{
				p_logger_->log(logger_type_, DEBUG, "Command '"+ cmd.command + "' is not G0/G1, skipping.  Gcode:" + cmd.gcode);
			}
			else if (!utilities::is_equal(p_cur_pos->z, p_pre_pos->z))
			{
				p_logger_->log(logger_type_, DEBUG, "Z axis position changed, cannot convert:" + cmd.gcode);
			}
			else if (p_cur_pos->is_relative)
			{
				p_logger_->log(logger_type_, DEBUG, "XYZ Axis is in relative mode, cannot convert:" + cmd.gcode);
			}
			else if (
				waiting_for_arc_ && !( 
					(p_pre_pos->get_current_extruder().is_extruding && extruder_current.is_extruding) ||
					(p_pre_pos->get_current_extruder().is_retracting && extruder_current.is_retracting)
				)
			)
			{
				std::string message = "Extruding or retracting state changed, cannot add point to current arc: " + cmd.gcode;
				if (verbose_logging_enabled_)
				{
					extruder previous_extruder = p_pre_pos->get_current_extruder();
					message.append(
						" - Verbose Info\n\tCurrent Position Info - Absolute E:" + utilities::to_string(extruder_current.e) +
						", Offset E:" + utilities::to_string(extruder_current.get_offset_e()) +
						", Mode:" + (p_cur_pos->is_extruder_relative_null ? "NULL" : p_cur_pos->is_extruder_relative ? "relative" : "absolute") +
						", Retraction: " + utilities::to_string(extruder_current.retraction_length) +
						", Extrusion: " + utilities::to_string(extruder_current.extrusion_length) +
						", Retracting: " + (extruder_current.is_retracting ? "True" : "False") +
						", Extruding: " + (extruder_current.is_extruding ? "True" : "False")
					);
					message.append(
						"\n\tPrevious Position Info - Absolute E:" + utilities::to_string(previous_extruder.e) +
						", Offset E:" + utilities::to_string(previous_extruder.get_offset_e()) +
						", Mode:" + (p_pre_pos->is_extruder_relative_null ? "NULL" : p_pre_pos->is_extruder_relative ? "relative" : "absolute") +
						", Retraction: " + utilities::to_string(previous_extruder.retraction_length) +
						", Extrusion: " + utilities::to_string(previous_extruder.extrusion_length) +
						", Retracting: " + (previous_extruder.is_retracting ? "True" : "False") +
						", Extruding: " + (previous_extruder.is_extruding ? "True" : "False")
					);
					p_logger_->log(logger_type_, VERBOSE, message);
				}
				else
				{
					p_logger_->log(logger_type_, DEBUG, message);
				}
				
			}
			else if (p_cur_pos->is_extruder_relative != p_pre_pos->is_extruder_relative)
			{
				p_logger_->log(logger_type_, DEBUG, "Extruder axis mode changed, cannot add point to current arc: " + cmd.gcode);
			}
			else if (waiting_for_arc_ && p_pre_pos->f != p_cur_pos->f)
			{
				p_logger_->log(logger_type_, DEBUG, "Feedrate changed, cannot add point to current arc: " + cmd.gcode);
			}
			else if (waiting_for_arc_ && p_pre_pos->feature_type_tag != p_cur_pos->feature_type_tag)
			{
				p_logger_->log(logger_type_, DEBUG, "Feature type changed, cannot add point to current arc: " + cmd.gcode);
			}
			else
			{
				// Todo:  Add all the relevant values
				p_logger_->log(logger_type_, DEBUG, "There was an unknown issue preventing the current point from being added to the arc: " + cmd.gcode);
			}
		}
	}
	if (!arc_added)
	{
		if (current_arc_.get_num_segments() < current_arc_.get_min_segments()) {
			if (debug_logging_enabled_ && !cmd.is_empty)
			{
				if (current_arc_.get_num_segments() != 0)
				{
					p_logger_->log(logger_type_, DEBUG, "Not enough segments, resetting. Gcode:" + cmd.gcode);
				}
				
			}
			waiting_for_arc_ = false;
			current_arc_.clear();
		}
		else if (waiting_for_arc_)
		{

			if (current_arc_.is_shape())
			{
				// increment our statistics
				points_compressed_ += current_arc_.get_num_segments()-1;
				arcs_created_++;
				//std::cout << "Arc shape found.\n";
				// Get the comment now, before we remove the previous comments
				std::string comment = get_comment_for_arc();
				// remove the same number of unwritten gcodes as there are arc segments, minus 1 for the start point
				// Which isn't a movement
				// note, skip the first point, it is the starting point
				for (int index = 0; index < current_arc_.get_num_segments() - 1; index++)
				{
					unwritten_commands_.pop_back();
				}
				// get the current absolute e coordinate of the previous position (the current position is not included in 
				// the arc) so we can make any adjustments that are necessary.
				double current_f = p_pre_pos->f;
				
				// IMPORTANT NOTE: p_cur_pos and p_pre_pos will NOT be usable beyond this point.
				p_pre_pos = NULL;
				p_cur_pos = NULL;
				// Undo the previous updates that will be turned into the arc, including the current position
				for (int index = 0; index < current_arc_.get_num_segments(); index++)
				{
					undo_commands_.push_back(p_source_position_->get_current_position_ptr()->command);
					p_source_position_->undo_update();
				}
				//position * p_undo_positions = p_source_position_->undo_update(current_arc_.get_num_segments());
				
				// Set the current feedrate if it is different, else set to 0 to indicate that no feedrate should be included
				if(p_source_position_->get_current_position_ptr()->f == current_f)
				{
					current_f = 0;
				}

				// Craete the arc gcode
				std::string gcode = get_arc_gcode(current_f, comment);

				if (debug_logging_enabled_)
				{
					p_logger_->log(logger_type_, DEBUG, "Arc created with " + std::to_string(current_arc_.get_num_segments()) + " segments: " + gcode);
				}

				// parse the arc gcode
				parsed_command new_command;
				bool parsed = parser_.try_parse_gcode(gcode.c_str(), new_command);
				if (!parsed)
				{
					if (error_logging_enabled_)
					{
						p_logger_->log_exception(logger_type_, "Unable to parse arc command!  Fatal Error.");
					}
					throw std::exception();
				}
				// update the position processor and add the command to the unwritten commands list
				p_source_position_->update(new_command, lines_processed_, gcodes_processed_, -1);
				unwritten_commands_.push_back(unwritten_command(p_source_position_->get_current_position_ptr()));
				
				// write all unwritten commands (if we don't do this we'll mess up absolute e by adding an offset to the arc)
				// including the most recent arc command BEFORE updating the absolute e offset
				write_unwritten_gcodes_to_file();

				// If the e values are not equal, use G91 to adjust the current absolute e position
				double difference = 0;
				double new_e_rel_relative = p_source_position_->get_current_position().get_current_extruder().e_relative;
				double old_e_relative = current_arc_.get_shape_e_relative();

				// See if any offset needs to be applied for absolute E coordinates
				if (
					!utilities::is_equal(new_e_rel_relative, old_e_relative))
				{
					// Calculate the difference between the original absolute e and 
					// change made by G2/G3
					difference = new_e_rel_relative - old_e_relative;
					// Adjust the absolute E offset based on the difference
					// We need to do this AFTER writing the modified gcode(arc), since the 
					// difference is based on that.
					absolute_e_offset_ += difference;
					if (debug_logging_enabled_)
					{
						p_logger_->log(logger_type_, DEBUG, "Adjusting absolute extrusion by " + utilities::to_string(difference) + "mm.  New Offset: " + utilities::to_string(difference));
					}
				}
				
				
				// Undo the arc update and re-apply the original commands to the position processor so that subsequent 
				// gcodes in the file are interpreted properly.  Do NOT add the most recent command
				// since it will be reprocessed
				p_source_position_->undo_update();
				
				for (int index = current_arc_.get_num_segments() - 1; index > 0; index--)
				{
					parsed_command cmd = undo_commands_.pop_back();
					p_source_position_->update(undo_commands_[index], lines_processed_, gcodes_processed_, -1);
				}
				undo_commands_.clear();
				// Now clear the arc and flag the processor as not waiting for an arc
				waiting_for_arc_ = false;
				current_arc_.clear();
				

				// Reprocess this line
				if (!is_end)
				{
					return process_gcode(cmd, false);
				}
				else
				{
					if (debug_logging_enabled_)
					{
						p_logger_->log(logger_type_, DEBUG, "Final arc created, exiting.");
					}
					return 0;
				}
					
			}
			else
			{
				if (debug_logging_enabled_)
				{
					p_logger_->log(logger_type_, DEBUG, "The current arc is not a valid arc, resetting.");
				}
				current_arc_.clear();
				waiting_for_arc_ = false;
			}
		}
		else if (debug_logging_enabled_)
		{
			p_logger_->log(logger_type_, DEBUG, "Could not add point to arc from gcode:" + cmd.gcode);
		}

	}
	if (clear_shapes)
	{
		waiting_for_arc_ = false;
		current_arc_.clear();
		// The current command is unwritten, add it.
		unwritten_commands_.push_back(unwritten_command(p_source_position_->get_current_position_ptr()));
	}
	else if (waiting_for_arc_ || !arc_added)
	{

		unwritten_commands_.push_back(unwritten_command(p_source_position_->get_current_position_ptr()));
		
	}
	if (!waiting_for_arc_)
	{
		write_unwritten_gcodes_to_file();
	}
	if (cmd.command == "G92")
	{
		// See if there is an E parameter
		for (unsigned int parameter_index = 0; parameter_index < cmd.parameters.size(); parameter_index++)
		{
			parsed_command_parameter param = cmd.parameters[parameter_index];
			if (param.name == "E")
			{
				absolute_e_offset_ = 0;
				if (debug_logging_enabled_)
				{
					p_logger_->log(logger_type_, DEBUG, "G92 found that set E axis, resetting absolute offset.");
				}
			}
		}
	}
	
	return lines_written;
}

std::string arc_welder::get_comment_for_arc()
{
	// build a comment string from the commands making up the arc
				// We need to start with the first command entered.
	int comment_index = unwritten_commands_.count() - (current_arc_.get_num_segments() - 1);
	std::string comment;
	for (; comment_index < unwritten_commands_.count(); comment_index++)
	{
		std::string old_comment = unwritten_commands_[comment_index].command.comment;
		if (old_comment != comment && old_comment.length() > 0)
		{
			if (comment.length() > 0)
			{
				comment += " - ";
			}
			comment += old_comment;
		}
	}
	return comment;
}

std::string arc_welder::create_g92_e(double absolute_e)
{
	std::stringstream stream;
	stream << std::fixed << std::setprecision(5);
	stream << "G92 E" << absolute_e;
	return stream.str();
}

int arc_welder::write_gcode_to_file(std::string gcode)
{
	output_file_ << utilities::trim(gcode) << "\n";
	//std::cout << utilities::trim(gcode) << "\n";
	return 1;
}

int arc_welder::write_unwritten_gcodes_to_file()
{
	int size = unwritten_commands_.count();
	std::string gcode_to_write;
	
	
	for (int index = 0; index < size; index++)
	{
		// The the current unwritten position and remove it from the list
		unwritten_command p = unwritten_commands_.pop_front();
		bool has_e_coordinate = false;
		std::string additional_comment = "";
		double old_e = p.offset_e;
		double new_e = old_e;
		if (!p.is_extruder_relative && utilities::greater_than(abs(absolute_e_offset_), 0.0) &&
			absolute_e_rewrite_commands_.find(p.command.command) != absolute_e_rewrite_commands_.end()
		){
			// handle any absolute extrusion shift
			// There is an offset, and we are in absolute E.  Rewrite the gcode
			parsed_command new_command = p.command;
			new_command.parameters.clear();
			has_e_coordinate = false;
			for (unsigned int index = 0; index < p.command.parameters.size(); index++)
			{
				parsed_command_parameter p_cur_param = p.command.parameters[index];
				if (p_cur_param.name == "E")
				{
					has_e_coordinate = true;
					if (p_cur_param.value_type == 'U')
					{
						p_cur_param.value_type = 'F';
					}
					new_e = p.offset_e + absolute_e_offset_;
					p_cur_param.double_value = new_e;
					
				}
				new_command.parameters.push_back(p_cur_param);
			}

			
			if (has_e_coordinate)
			{
				p.command = new_command;
			}
		}
		
		write_gcode_to_file(p.to_string(has_e_coordinate, additional_comment));
		
	}
	
	return size;
}

std::string arc_welder::get_arc_gcode(double f, const std::string comment)
{
	// Write gcode to file
	std::string gcode;
	position* p_new_current_pos = p_source_position_->get_current_position_ptr();
	
	if (p_new_current_pos->is_extruder_relative)
	{
		gcode = current_arc_.get_shape_gcode_relative(f);
	}
	else
	{
		// Make sure to add the absoulte e offset
		gcode = current_arc_.get_shape_gcode_absolute(f, p_new_current_pos->get_current_extruder().get_offset_e());
	}
	if (comment.length() > 0)
	{
		gcode += ";" + comment;
	}
	return gcode;
	
}
