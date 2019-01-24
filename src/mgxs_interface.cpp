#include "openmc/mgxs_interface.h"

#include <string>
#include <unordered_set>

#include "openmc/cross_sections.h"
#include "openmc/error.h"
#include "openmc/file_utils.h"
#include "openmc/geometry_aux.h"
#include "openmc/hdf5_interface.h"
#include "openmc/material.h"
#include "openmc/math_functions.h"
#include "openmc/nuclide.h"
#include "openmc/settings.h"


namespace openmc {

//==============================================================================
// Global variable definitions
//==============================================================================

namespace data {

std::vector<double> energy_bins;
std::vector<double> energy_bin_avg;
std::vector<double> rev_energy_bins;

} // namesapce data

//==============================================================================
// Mgxs data loading interface methods
//==============================================================================

void read_mgxs()
{
  // Check if MGXS Library exists
  if (!file_exists(settings::path_cross_sections)) {
    // Could not find MGXS Library file
    fatal_error("Cross sections HDF5 file '" + settings::path_cross_sections +
      "' does not exist.");
  }

  write_message("Loading cross section data...", 5);

  // Get temperatures
  std::vector<std::vector<double>> nuc_temps(data::nuclide_map.size());
  std::vector<std::vector<double>> dummy;
  get_temperatures(nuc_temps, dummy);

  // Open file for reading
  hid_t file_id = file_open(settings::path_cross_sections, 'r');

  // Read filetype
  std::string type;
  read_attribute(file_id, "filetype", type);
  if (type != "mgxs") {
    fatal_error("Provided MGXS Library is not a MGXS Library file.");
  }

  // Read revision number for the MGXS Library file and make sure it matches
  // with the current version
  std::array<int, 2> array;
  read_attribute(file_id, "version", array);
  if (array != VERSION_MGXS_LIBRARY) {
    fatal_error("MGXS Library file version does not match current version "
      "supported by OpenMC.");
  }

  // ==========================================================================
  // READ ALL MGXS CROSS SECTION TABLES

  std::unordered_set<std::string> already_read;

  // Build vector of nuclide names
  std::vector<std::string> nuclide_names(data::nuclide_map.size());
  for (const auto& kv : data::nuclide_map) {
    nuclide_names[kv.second] = kv.first;
  }

  // Loop over all files
  for (const auto& mat : model::materials) {
    for (int i_nuc : mat->nuclide_) {
      std::string& name = nuclide_names[i_nuc];

      if (already_read.find(name) == already_read.end()) {
        add_mgxs_c(file_id, name, nuc_temps[i_nuc]);
        already_read.insert(name);
      }

      if (data::nuclides_MG[i_nuc].fissionable) {
        mat->fissionable_ = true;
      }
    }
  }

  file_close(file_id);
}

void
add_mgxs_c(hid_t file_id, const std::string& name,
  const std::vector<double>& temperature)
{
  write_message("Loading " + std::string(name) + " data...", 6);

  // Check to make sure cross section set exists in the library
  hid_t xs_grp;
  if (object_exists(file_id, name.c_str())) {
    xs_grp = open_group(file_id, name.c_str());
  } else {
    fatal_error("Data for " + std::string(name) + " does not exist in "
                + "provided MGXS Library");
  }

  data::nuclides_MG.emplace_back(xs_grp, temperature);
  close_group(xs_grp);
}

//==============================================================================

bool
query_fissionable_c(int n_nuclides, const int i_nuclides[])
{
  bool result = false;
  for (int n = 0; n < n_nuclides; n++) {
    if (data::nuclides_MG[i_nuclides[n] - 1].fissionable) result = true;
  }
  return result;
}

//==============================================================================

void
create_macro_xs_c(const char* mat_name, int n_nuclides, const int i_nuclides[],
     int n_temps, const double temps[], const double atom_densities[],
     double tolerance, int& method)
{
  if (n_temps > 0) {
    // // Convert temps to a vector
    std::vector<double> temperature(temps, temps + n_temps);

    // Convert atom_densities to a vector
    std::vector<double> atom_densities_vec(atom_densities,
         atom_densities + n_nuclides);

    // Build array of pointers to nuclides_MG's Mgxs objects needed for this
    // material
    std::vector<Mgxs*> mgxs_ptr(n_nuclides);
    for (int n = 0; n < n_nuclides; n++) {
      mgxs_ptr[n] = &data::nuclides_MG[i_nuclides[n] - 1];
    }

    data::macro_xs.emplace_back(mat_name, temperature, mgxs_ptr, atom_densities_vec);
  } else {
    // Preserve the ordering of materials by including a blank entry
    data::macro_xs.emplace_back();
  }
}

//==============================================================================

void read_mg_cross_sections_header_c(hid_t file_id)
{
  ensure_exists(file_id, "energy_groups", true);
  read_attribute(file_id, "energy_groups", data::num_energy_groups);

  ensure_exists(file_id, "group structure", true);
  read_attribute(file_id, "group structure", data::rev_energy_bins);

  // Reverse energy bins
  std::copy(data::rev_energy_bins.crbegin(), data::rev_energy_bins.crend(),
    std::back_inserter(data::energy_bins));

  // Create average energies
  for (int i = 0; i < data::energy_bins.size() - 1; ++i) {
    data::energy_bin_avg.push_back(0.5*(data::energy_bins[i] + data::energy_bins[i+1]));
  }

  // Add entries into libraries for MG data
  auto names = group_names(file_id);
  if (names.empty()) {
    fatal_error("At least one MGXS data set must be present in mgxs "
      "library file!");
  }

  for (auto& name : names) {
    Library lib {};
    lib.type_ = Library::Type::neutron;
    lib.materials_.push_back(name);
    data::libraries.push_back(lib);
  }
}

//==============================================================================
// Mgxs tracking/transport/tallying interface methods
//==============================================================================

void
calculate_xs_c(int i_mat, int gin, double sqrtkT, const double uvw[3],
     double& total_xs, double& abs_xs, double& nu_fiss_xs)
{
  data::macro_xs[i_mat - 1].calculate_xs(gin - 1, sqrtkT, uvw, total_xs, abs_xs,
       nu_fiss_xs);
}

//==============================================================================

double
get_nuclide_xs_c(int index, int xstype, int gin, int* gout, double* mu, int* dg)
{
  int gout_c;
  int* gout_c_p;
  int dg_c;
  int* dg_c_p;
  if (gout != nullptr) {
    gout_c = *gout - 1;
    gout_c_p = &gout_c;
  } else {
    gout_c_p = gout;
  }
  if (dg != nullptr) {
    dg_c = *dg - 1;
    dg_c_p = &dg_c;
  } else {
    dg_c_p = dg;
  }
  return data::nuclides_MG[index - 1].get_xs(xstype, gin - 1, gout_c_p, mu, dg_c_p);
}

//==============================================================================

double
get_macro_xs_c(int index, int xstype, int gin, int* gout, double* mu, int* dg)
{
  int gout_c;
  int* gout_c_p;
  int dg_c;
  int* dg_c_p;
  if (gout != nullptr) {
    gout_c = *gout - 1;
    gout_c_p = &gout_c;
  } else {
    gout_c_p = gout;
  }
  if (dg != nullptr) {
    dg_c = *dg - 1;
    dg_c_p = &dg_c;
  } else {
    dg_c_p = dg;
  }
  return data::macro_xs[index - 1].get_xs(xstype, gin - 1, gout_c_p, mu, dg_c_p);
}

//==============================================================================

void
set_nuclide_angle_index_c(int index, const double uvw[3])
{
  // Update the values
  data::nuclides_MG[index - 1].set_angle_index(uvw);
}

//==============================================================================

void
set_macro_angle_index_c(int index, const double uvw[3])
{
  // Update the values
  data::macro_xs[index - 1].set_angle_index(uvw);
}

//==============================================================================

void
set_nuclide_temperature_index_c(int index, double sqrtkT)
{
  // Update the values
  data::nuclides_MG[index - 1].set_temperature_index(sqrtkT);
}

//==============================================================================
// General Mgxs methods
//==============================================================================

void
get_name_c(int index, int name_len, char* name)
{
  // First blank out our input string
  std::string str(name_len - 1, ' ');
  std::strcpy(name, str.c_str());

  // Now get the data and copy to the C-string
  str = data::nuclides_MG[index - 1].name;
  std::strcpy(name, str.c_str());

  // Finally, remove the null terminator
  name[std::strlen(name)] = ' ';
}

//==============================================================================

double
get_awr_c(int index)
{
  return data::nuclides_MG[index - 1].awr;
}

} // namespace openmc
