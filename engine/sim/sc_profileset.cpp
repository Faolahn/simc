// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "simulationcraft.hpp"
#include "sc_profileset.hpp"

namespace profileset
{
void insert_data( highchart::bar_chart_t&   chart,
                  const std::string&        name,
                  const color::rgb&         c,
                  const statistical_data_t& data,
                  bool                      baseline )
{
  js::sc_js_t entry;

  if ( baseline )
  {
    entry.set( "color", "#AA0000" );
    entry.set( "dataLabels.color", "#AA0000" );
    entry.set( "dataLabels.style.fontWeight", "bold" );
  }

  entry.set( "name", name );
  entry.set( "y", util::round( data.median ) );

  chart.add( "series.0.data", entry );

  js::sc_js_t boxplot_entry;
  boxplot_entry.set( "name", name );
  boxplot_entry.set( "low", data.min );
  boxplot_entry.set( "q1", data.first_quartile );
  boxplot_entry.set( "median", data.median );
  boxplot_entry.set( "mean", data.mean );
  boxplot_entry.set( "q3", data.third_quartile );
  boxplot_entry.set( "high", data.max );

  if ( baseline )
  {
    color::rgb c( "AA0000" );
    boxplot_entry.set( "color", c.dark( .5 ).opacity( .5 ).str() );
    boxplot_entry.set( "fillColor", c.dark( .75 ).opacity( .5 ).rgb_str() );
  }
  else
  {
    boxplot_entry.set( "fillColor", c.dark( .75 ).opacity( .5 ).rgb_str() );
  }

  chart.add( "series.1.data", boxplot_entry );
}

sim_control_t* profile_set_t::create_sim_options( const sim_control_t*            original,
                                                  const std::vector<std::string>& opts )
{
  if ( original == nullptr )
  {
    return nullptr;
  }

  auto new_options = new sim_control_t( *original );

  try
  {
    new_options -> options.parse_args( opts );
  }
  catch ( const std::exception& e ) {
    std::cerr << "ERROR! Incorrect option format: " << e.what() << std::endl;
    delete new_options;
    return nullptr;
  }

  return new_options;
}

profile_set_t::profile_set_t( const std::string& name, sim_control_t* opts, bool has_output ) :
  m_name( name ), m_options( opts ), m_has_output( has_output )
{
}

sim_control_t* profile_set_t::options() const
{
  return m_options;
}

profile_set_t::~profile_set_t()
{
  delete m_options;
}

void profile_set_t::done()
{
  // Deleting the profileset simulator object will free the sim_control_t object, so nullptr ours to
  // avoid a double-free.
  m_options = nullptr;
}

bool profilesets_t::validate( sim_t* ps_sim )
{
  if ( ps_sim -> player_no_pet_list.size() > 1 )
  {
    ps_sim -> errorf( "Profileset simulations must have only one actor" );
    return false;
  }

  return true;
}

bool profilesets_t::parse( sim_t* sim )
{
  if ( sim -> profileset_map.size() == 0 )
  {
    return true;
  }

  if ( ! validate( sim ) )
  {
    return false;
  }

  auto original_control = sim -> control;

  // Generate a copy of the original control, and remove any and all profileset. options from it
  m_original = std::unique_ptr<sim_control_t>( new sim_control_t( *original_control ) );

  // Remove profileset.foo options. Leave "profileset_metric", it is needed in the child sims
  auto it = m_original -> options.begin();
  while ( it != m_original -> options.end() )
  {
    const auto& opt = *it;
    if ( util::str_in_str_ci( opt.name, "profileset." ) )
    {
      it = m_original -> options.erase( it );
    }
    else
    {
      ++it;
    }
  }

  for ( auto it = sim -> profileset_map.begin(); it != sim -> profileset_map.end(); ++it )
  {
    if ( sim -> canceled )
    {
      return false;
    }

    auto control = profile_set_t::create_sim_options( m_original.get(), it -> second );
    if ( control == nullptr )
    {
      return false;
    }

    auto has_output_opts = range::find_if( it -> second, []( const std::string& opt ) {
      return opt.find( "output", 0, opt.find( "=" ) ) != std::string::npos ||
             opt.find( "html", 0, opt.find( "=" ) ) != std::string::npos ||
             opt.find( "xml", 0, opt.find( "=" ) ) != std::string::npos ||
             opt.find( "json2", 0, opt.find( "=" ) ) != std::string::npos;
    } ) != it -> second.end();

    sim -> control = control;

    // Test that profileset options are OK, up to the simulation initialization
    try
    {
      auto test_sim = new sim_t( sim );
      auto ret = test_sim -> init();
      if ( ! ret || ! validate( test_sim ) )
      {
        sim -> control = original_control;
        delete test_sim;
        return false;
      }

      delete test_sim;
    }
    catch ( const std::exception& e )
    {
      std::cerr <<  "ERROR! Profileset '" << it -> first << "' Setup failure: "
                << e.what() << std::endl;
      sim -> control = original_control;
      return false;
    }

    m_profilesets.push_back( std::unique_ptr<profile_set_t>(
        new profile_set_t( it -> first, control, has_output_opts ) ) );
    std::cerr << "Generating profileset " << it -> first << std::endl;
  }

  sim -> control = original_control;

  return true;
}

bool profilesets_t::iterate( sim_t* parent )
{
  auto original_opts = parent -> control;

  for ( auto& set : m_profilesets )
  {
    parent -> control = set -> options();

    auto profile_sim = new sim_t( parent );

    // Reset random seed for the profileset sims
    profile_sim -> seed = 0;
    profile_sim -> profileset_enabled = true;
    profile_sim -> report_details = 0;
    profile_sim -> set_sim_base_str( set -> name() );

    auto ret = profile_sim -> execute();
    if ( ret && set -> has_output() )
    {
      report::print_suite( profile_sim );
    }

    set -> done();

    if ( ret == false || profile_sim -> is_canceled() )
    {
      parent -> control = original_opts;
      delete profile_sim;
      return false;
    }

    const auto player = profile_sim -> player_no_pet_list.data().front();
    auto progress = profile_sim -> progress( nullptr, 0 );
    auto data = metric_data( player );

    set -> result()
      .min( data.min )
      .first_quartile( data.first_quartile )
      .median( data.median )
      .mean( data.mean )
      .third_quartile( data.third_quartile )
      .max( data.max )
      .stddev( data.std_dev )
      .iterations( progress.current_iterations );

    delete profile_sim;
  }

  parent -> control = original_opts;

  return true;
}

int profilesets_t::max_name_length() const
{
  size_t len = 0;

  range::for_each( m_profilesets, [ &len ]( const profileset_entry_t& profileset ) {
    if ( profileset -> name().size() > len )
    {
      len = profileset -> name().size();
    }
  } );

  return len;
}

void profilesets_t::output( const sim_t& sim, js::JsonOutput& root ) const
{
  root[ "metric" ] = util::scale_metric_type_string( sim.profileset_metric );

  auto& results = root[ "results" ].make_array();

  range::for_each( m_profilesets, [ &results ]( const profileset_entry_t& profileset ) {
    if ( profileset -> result().mean() == 0 )
    {
      return;
    }

    auto obj = results.add();
    const auto& result = profileset -> result();

    obj[ "name" ] = profileset -> name();
    obj[ "mean" ] = result.mean();
    obj[ "min" ] = result.min();
    obj[ "max" ] = result.max();
    obj[ "stddev" ] = result.stddev();

    if ( result.median() != 0 )
    {
      obj[ "median" ] = result.median();
      obj[ "first_quartile" ] = result.first_quartile();
      obj[ "third_quartile" ] = result.third_quartile();
    }

    obj[ "iterations" ] = as<uint64_t>( result.iterations() );
  } );
}

void profilesets_t::output( const sim_t& sim, FILE* out ) const
{
  if ( m_profilesets.size() == 0 )
  {
    return;
  }

  util::fprintf( out, "\n\nProfilesets (median %s):\n",
    util::scale_metric_type_string( sim.profileset_metric ) );

  std::vector<const profile_set_t*> results;
  generate_sorted_profilesets( results );

  range::for_each( results, [ out ]( const profile_set_t* profileset ) {
    util::fprintf( out, "    %-10.3f : %s\n",
      profileset -> result().median(), profileset -> name().c_str() );
  } );
}

void profilesets_t::output( const sim_t& sim, io::ofstream& out ) const
{
  if ( m_profilesets.size() == 0 )
  {
    return;
  }

  out << "<div class=\"section\">\n";
  out << "<h2 class=\"toggle\">Profile sets</h2>\n";
  out << "<div class=\"toggle-content hide\">\n";

  generate_chart( sim, out );

  out << "</div>";
  out << "</div>";
}

void profilesets_t::generate_sorted_profilesets( std::vector<const profile_set_t*>& out ) const
{
  range::transform( m_profilesets, std::back_inserter( out ), []( const profileset_entry_t& p ) {
    return p.get();
  } );

  // Sort to descending with mean value
  range::sort( out, []( const profile_set_t* l, const profile_set_t* r ) {
    return l -> result().median() > r -> result().median();
  } );
}

bool profilesets_t::generate_chart( const sim_t& sim, io::ofstream& out ) const
{
  size_t chart_id = 0;

  // Baseline data, insert into the correct position in the for loop below
  const auto baseline = sim.player_no_pet_list.data().front();
  auto baseline_data = metric_data( baseline );
  auto inserted = false;
  // Bar color
  const auto& c = color::class_color( sim.player_no_pet_list.data().front() -> type );
  std::string chart_name = util::scale_metric_type_string( sim.profileset_metric );

  std::vector<const profile_set_t*> results;
  generate_sorted_profilesets( results );

  while ( chart_id * MAX_CHART_ENTRIES < m_profilesets.size() )
  {
    highchart::bar_chart_t profileset( "profileset-" + util::to_string( chart_id ), sim );

    auto base_offset = chart_id * MAX_CHART_ENTRIES;

    profileset.set( "series.0.name", chart_name );
    profileset.set( "series.1.type", "boxplot" );
    profileset.set( "series.1.name", chart_name );
    profileset.set( "yAxis.gridLineWidth", 0 );
    profileset.set( "xAxis.offset", 80 );
    profileset.set_title( "Profile sets (median " + chart_name + ")" );
    profileset.set( "subtitle.text", "Baseline in red" );
    profileset.set( "subtitle.style.color", "#AA0000" );
    profileset.set_yaxis_title( "Median " + chart_name );
    profileset.width_ = 1150;
    profileset.height_ = 24 * std::min( as<size_t>( MAX_CHART_ENTRIES + 1 ), results.size() - base_offset + 1 ) + 75;

    profileset.add( "colors", c.str() );
    profileset.add( "colors", c.dark( .5 ).opacity( .5 ).str() );
    profileset.set( "plotOptions.boxplot.whiskerLength", "85%" );
    profileset.set( "plotOptions.boxplot.whiskerWidth", 1.5 );
    profileset.set( "plotOptions.boxplot.medianWidth", 1 );
    profileset.set( "plotOptions.boxplot.pointWidth", 18 );
    profileset.set( "plotOptions.boxplot.tooltip.pointFormat",
      "Maximum: {point.high}<br/>"
      "Upper quartile: {point.q3}<br/>"
      "Mean: {point.mean:,.1f}<br/>"
      "Median: {point.median}<br/>"
      "Lower quartile: {point.q1}<br/>"
      "Minimum: {point.low}<br/>"
    );

    profileset.set( "plotOptions.bar.dataLabels.crop", false );
    profileset.set( "plotOptions.bar.dataLabels.overflow", "none" );
    profileset.set( "plotOptions.bar.dataLabels.inside", true );
    profileset.set( "plotOptions.bar.dataLabels.enabled", true );
    profileset.set( "plotOptions.bar.dataLabels.align", "left" );
    profileset.set( "plotOptions.bar.dataLabels.shadow", false );
    profileset.set( "plotOptions.bar.dataLabels.x", -80 );
    profileset.set( "plotOptions.bar.dataLabels.color", c.str() );
    profileset.set( "plotOptions.bar.dataLabels.verticalAlign", "middle" );
    profileset.set( "plotOptions.bar.dataLabels.style.fontSize", "10pt" );
    profileset.set( "plotOptions.bar.dataLabels.style.fontWeight", "none" );
    profileset.set( "plotOptions.bar.dataLabels.style.textShadow", "none" );

    profileset.set( "xAxis.labels.style.color", c.str() );
    profileset.set( "xAxis.labels.style.whiteSpace", "nowrap" );
    profileset.set( "xAxis.labels.style.fontSize", "10pt" );
    profileset.set( "xAxis.labels.padding", 2 );
    profileset.set( "xAxis.lineColor", c.str() );

    // Custom formatter for X axis so we can get baseline colored different
    std::string functor = "function () {";
    functor += "  if (this.value === '" + baseline -> name_str + "') {";
    functor += "    return '<span style=\"color:#AA0000;font-weight:bold;\">' + this.value + '</span>';";
    functor += "  }";
    functor += "  else {";
    functor += "    return this.value;";
    functor += "  }";
    functor += "}";

    profileset.set( "xAxis.labels.formatter", functor );
    profileset.value( "xAxis.labels.formatter" ).SetRawOutput( true );

    for ( size_t i = base_offset, end = std::min( base_offset + MAX_CHART_ENTRIES, results.size() ); i < end; ++i )
    {
      const auto set = results[ i ];

      if ( ! inserted && set -> result().median() <= baseline_data.median )
      {
        insert_data( profileset, sim.player_no_pet_list.data().front() -> name(), c, baseline_data, true );
        inserted = true;
      }

      insert_data( profileset, set -> name(), c, set -> result().statistical_data(), false );
    }

    if ( inserted == false )
    {
      insert_data( profileset, sim.player_no_pet_list.data().front() -> name(), c, baseline_data, true );
    }

    out << profileset.to_string();
    ++chart_id;
    inserted = false;
  }

  return true;
}

void create_options( sim_t* sim )
{
  sim -> add_option( opt_map_list( "profileset.", sim -> profileset_map ) );
  sim -> add_option( opt_func( "profileset_metric", []( sim_t*             sim,
                                                        const std::string&,
                                                        const std::string& value ) {
    scale_metric_e metric = util::parse_scale_metric( value );
    if ( metric == SCALE_METRIC_NONE )
    {
      return false;
    }

    sim -> profileset_metric = metric;
    return true;
  } ) );
}

statistical_data_t collect( const extended_sample_data_t& c )
{
  return { c.min(), c.percentile( 0.25 ), c.mean(),
           c.percentile( 0.5 ), c.percentile( 0.75 ), c.max(), c.std_dev };
}

statistical_data_t metric_data( const player_t* player )
{
  const auto& d = player -> collected_data;

  switch ( player -> sim -> profileset_metric )
  {
    case SCALE_METRIC_DPS:       return collect( d.dps );
    case SCALE_METRIC_DPSE:      return collect( d.dpse );
    case SCALE_METRIC_HPS:       return collect( d.hps );
    case SCALE_METRIC_HPSE:      return collect( d.hpse );
    case SCALE_METRIC_APS:       return collect( d.aps );
    case SCALE_METRIC_DPSP:      return collect( d.prioritydps );
    case SCALE_METRIC_DTPS:      return collect( d.dtps );
    case SCALE_METRIC_DMG_TAKEN: return collect( d.dmg_taken );
    case SCALE_METRIC_HTPS:      return collect( d.htps );
    case SCALE_METRIC_TMI:       return collect( d.theck_meloree_index );
    case SCALE_METRIC_ETMI:      return collect( d.effective_theck_meloree_index );
    case SCALE_METRIC_DEATHS:    return collect( d.deaths );
    case SCALE_METRIC_HAPS:
    {
      auto hps = collect( d.hps );
      auto aps = collect( d.aps );
      return {
        hps.min + aps.min,
        hps.first_quartile + aps.first_quartile,
        hps.mean + aps.mean,
        hps.median + aps.median,
        hps.third_quartile + aps.first_quartile,
        hps.max + aps.max,
        sqrt( d.aps.mean_variance + d.hps.mean_variance )
      };
    }
    default:                     return { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
  }
}

} /* Namespace profileset ends */