/*
	SObjectizer 5.
*/

#include <so_5/agent.hpp>
#include <so_5/mbox.hpp>
#include <so_5/enveloped_msg.hpp>
#include <so_5/environment.hpp>
#include <so_5/send_functions.hpp>

#include <so_5/impl/internal_env_iface.hpp>
#include <so_5/impl/coop_private_iface.hpp>

#include <so_5/impl/delivery_filter_storage.hpp>
#include <so_5/impl/msg_tracing_helpers.hpp>
#include <so_5/impl/process_unhandled_exception.hpp>
#include <so_5/impl/std_message_sinks.hpp>
#include <so_5/impl/subscription_storage_iface.hpp>

#include <so_5/impl/enveloped_msg_details.hpp>

#include <so_5/details/abort_on_fatal_error.hpp>

#include <so_5/spinlocks.hpp>

#include <algorithm>
#include <sstream>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace so_5
{

//
// agent_identity_t::pointer_only_t
//
std::array<char, agent_identity_t::pointer_only_t::c_string_size>
agent_identity_t::pointer_only_t::make_c_string() const noexcept
	{
		static constexpr std::array<char, 16> hex_symbols{
			'0', '1', '2', '3', '4', '5', '6', '7',
			'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
		};

		std::array<char, c_string_size> result;
		auto it = std::copy(
				c_string_prefix.begin(),
				c_string_prefix.end(),
				result.begin() );

		// NOTE: this code won't compile if there is no std::uintptr_t type,
		// but we don't care about such platforms at the moment.
		std::uintptr_t ptr_as_uint =
				reinterpret_cast<std::uintptr_t>(m_pointer_value);

		// Handle by 4 bits portions from the most significant bit.
		// Leading zeros are not skipped (for the simplicity of implementation).
		constexpr unsigned int half_octet = 4u;
		unsigned bits_to_process = sizeof(void *) * (half_octet * 2u);
		while( bits_to_process >= half_octet )
			{
				const auto index = static_cast<std::size_t>(
						(ptr_as_uint >> (bits_to_process - half_octet)) & 0x0Fu
					);

				*(it++) = hex_symbols[ index ];

				bits_to_process -= half_octet;
			}

		it = std::copy( c_string_suffix.begin(), c_string_suffix.end(), it );
		*it = 0;

		return result;
	}

namespace
{

[[nodiscard]]
unsigned int
ensure_valid_agent_name_length( std::size_t length )
	{
		if( 0u == length )
			SO_5_THROW_EXCEPTION( rc_empty_agent_name,
					"Name of an agent can't be empty" );

		constexpr std::size_t max_unit_value =
				std::numeric_limits<unsigned int>::max();
		if( max_unit_value < length )
			SO_5_THROW_EXCEPTION( rc_empty_agent_name,
					"Name of an agent is too long (length should fit "
					"into unsigned int)" );

		return static_cast<unsigned int>( length );
	}

} /* namespace anonymous */

//
// name_for_agent_t
//
name_for_agent_t::name_for_agent_t()
	:	m_length{}
	{}

name_for_agent_t::name_for_agent_t( std::string_view value )
	:	m_length{ ensure_valid_agent_name_length( value.size() ) }
	{
		m_value.reset( new char[ value.size() ] );
		std::copy( std::begin(value), std::end(value), m_value.get() );
	}

name_for_agent_t::name_for_agent_t( const name_for_agent_t & other )
	:	m_length{ other.m_length }
	{
		if( other.has_value() )
			{
				m_value.reset( new char[ m_length ] );
				std::copy(
						other.m_value.get(),
						other.m_value.get() + m_length,
						m_value.get() );
			}
	}

name_for_agent_t &
name_for_agent_t::operator=( const name_for_agent_t & other )
	{
		name_for_agent_t tmp{ other };
		swap( *this, tmp );
		return *this;
	}

name_for_agent_t::name_for_agent_t( name_for_agent_t && other ) noexcept
	:	m_value{ std::exchange( other.m_value, std::unique_ptr< char[] >{} ) }
	,	m_length{ std::exchange( other.m_length, 0u ) }
	{}

name_for_agent_t &
name_for_agent_t::operator=( name_for_agent_t && other ) noexcept
	{
		name_for_agent_t tmp{ std::move(other) };
		swap( *this, tmp );
		return *this;
	}

SO_5_FUNC void
swap( name_for_agent_t & a, name_for_agent_t & b ) noexcept
	{
		using std::swap;
		swap( a.m_value, b.m_value );
		swap( a.m_length, b.m_length );
	}

name_for_agent_t::~name_for_agent_t() = default;

std::string_view
name_for_agent_t::as_string_view() const
	{
		std::string_view result;
		if( has_value() )
			result = std::string_view{ m_value.get(), m_length };
		return result;
	}

bool
name_for_agent_t::has_value() const noexcept
	{
		return 0u != m_length;
	}

namespace
{

/*!
 * \since
 * v.5.4.0
 *
 */
std::string
create_anonymous_state_name( const agent_t * agent, const state_t * st )
	{
		std::ostringstream ss;
		ss << "<state:target=" << agent << ":this=" << st << ">";
		return ss.str();
	}

} /* namespace anonymous */

// NOTE: Implementation of state_t is moved to that file in v.5.4.0.

//
// state_t::time_limit_t
//
struct state_t::time_limit_t
{
	using msg_timeout = so_5::details::msg_state_timeout;

	duration_t m_limit;
	const state_t & m_state_to_switch;

	mbox_t m_unique_mbox;
	timer_id_t m_timer;

	time_limit_t(
		duration_t limit,
		const state_t & state_to_switch )
		:	m_limit( limit )
		,	m_state_to_switch( state_to_switch )
	{}

	void
	set_up_limit_for_agent(
		agent_t & agent,
		const state_t & current_state ) noexcept
	{
		// Because this method is called from on_enter handler it can't
		// throw exceptions. Any exception will lead to abort of the application.
		// So we don't care about exception safety.
		so_5::details::invoke_noexcept_code( [&] {

			// New unique mbox is necessary for time limit.
			m_unique_mbox = impl::internal_env_iface_t{ agent.so_environment() }
					// A new MPSC mbox will be used for that.
					.create_limitless_mpsc_mbox(
							// New MPSC mbox will be directly connected to target agent.
							agent );

			// A subscription must be created for msg_timeout signal.
			agent.so_subscribe( m_unique_mbox )
					.in( current_state )
					.event( [&agent, this](mhood_t<msg_timeout>) {
						agent.so_change_state( m_state_to_switch );
					} );

			// Delayed timeout signal must be sent.
			m_timer = send_periodic< msg_timeout >(
					m_unique_mbox,
					m_limit,
					duration_t::zero() );
		} );
	}

	void
	drop_limit_for_agent(
		agent_t & agent,
		const state_t & current_state ) noexcept
	{
		// Because this method is called from on_exit handler it can't
		// throw exceptions. Any exception will lead to abort of the application.
		// So we don't care about exception safety.
		so_5::details::invoke_noexcept_code( [&] {
			m_timer.release();

			if( m_unique_mbox )
			{
				// Old subscription must be removed.
				agent.so_drop_subscription< msg_timeout >(
						m_unique_mbox, current_state );
				// Unique mbox is no more needed.
				m_unique_mbox = mbox_t{};
			}
		} );
	}
};

//
// state_t
//

state_t::state_t(
	agent_t * target_agent,
	std::string state_name,
	state_t * parent_state,
	std::size_t nested_level,
	history_t state_history )
	:	m_target_agent{ target_agent }
	,	m_state_name( std::move(state_name) )
	,	m_parent_state{ parent_state }
	,	m_initial_substate{ nullptr }
	,	m_state_history{ state_history }
	,	m_last_active_substate{ nullptr }
	,	m_nested_level{ nested_level }
	,	m_substate_count{ 0 }
{
	if( parent_state )
	{
		// We should check the deep of nested states.
		if( m_nested_level >= max_deep )
			SO_5_THROW_EXCEPTION( rc_state_nesting_is_too_deep,
					"max nesting deep for agent states is " +
					std::to_string( max_deep ) );

		// Now we can safely mark parent state as composite.
		parent_state->m_substate_count += 1;
	}
}

state_t::state_t(
	agent_t * agent )
	:	state_t{ agent, history_t::none }
{
}

state_t::state_t(
	agent_t * agent,
	history_t state_history )
	:	state_t{ agent, std::string(), nullptr, 0, state_history }
{
}

state_t::state_t(
	agent_t * agent,
	std::string state_name )
	:	state_t{ agent, std::move(state_name), history_t::none }
{}

state_t::state_t(
	agent_t * agent,
	std::string state_name,
	history_t state_history )
	:	state_t{ agent, std::move(state_name), nullptr, 0, state_history }
{}

state_t::state_t(
	initial_substate_of parent )
	:	state_t{ parent, std::string(), history_t::none }
{} 

state_t::state_t(
	initial_substate_of parent,
	std::string state_name )
	:	state_t{ parent, std::move(state_name), history_t::none }
{}

state_t::state_t(
	initial_substate_of parent,
	std::string state_name,
	history_t state_history )
	:	state_t{
			parent.m_parent_state->m_target_agent,
			std::move(state_name),
			parent.m_parent_state,
			parent.m_parent_state->m_nested_level + 1,
			state_history }
{
	if( m_parent_state->m_initial_substate )
		SO_5_THROW_EXCEPTION( rc_initial_substate_already_defined,
				"initial substate for state " + m_parent_state->query_name() +
				" is already defined: " +
				m_parent_state->m_initial_substate->query_name() );

	m_parent_state->m_initial_substate = this;
}

state_t::state_t(
	substate_of parent )
	:	state_t{ parent, std::string(), history_t::none }
{}

state_t::state_t(
	substate_of parent,
	std::string state_name )
	:	state_t{ parent, std::move(state_name), history_t::none }
{}

state_t::state_t(
	substate_of parent,
	std::string state_name,
	history_t state_history )
	:	state_t{
			parent.m_parent_state->m_target_agent,
			std::move(state_name),
			parent.m_parent_state,
			parent.m_parent_state->m_nested_level + 1,
			state_history }
{}

state_t::state_t(
	state_t && other )
	:	m_target_agent( other.m_target_agent )
	,	m_state_name( std::move( other.m_state_name ) )
	,	m_parent_state{ other.m_parent_state }
	,	m_initial_substate{ other.m_initial_substate }
	,	m_state_history{ other.m_state_history }
	,	m_last_active_substate{ other.m_last_active_substate }
	,	m_nested_level{ other.m_nested_level }
	,	m_substate_count{ other.m_substate_count }
	,	m_on_enter{ std::move(other.m_on_enter) }
	,	m_on_exit{ std::move(other.m_on_exit) }
{
	if( m_parent_state && m_parent_state->m_initial_substate == &other )
		m_parent_state->m_initial_substate = this;
}

state_t::~state_t()
{
}

bool
state_t::operator == ( const state_t & state ) const noexcept
{
	return &state == this;
}

std::string
state_t::query_name() const
{
	auto getter = [this]() -> std::string {
		if( m_state_name.empty() )
			return create_anonymous_state_name( m_target_agent, this );
		else
			return m_state_name;
	};

	if( m_parent_state )
		return m_parent_state->query_name() + "." + getter();
	else
		return getter();
}

namespace {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif

/*!
 * \since
 * v.5.4.0
 *
 * \brief A special object for the state in which agent is awaiting
 * for deregistration after unhandled exception.
 *
 * This object will be shared between all agents.
 */
const state_t awaiting_deregistration_state(
		nullptr, "<AWAITING_DEREGISTRATION_AFTER_UNHANDLED_EXCEPTION>" );

/*!
 * \since
 * v.5.5.21
 *
 * \brief A special object to be used as state for make subscriptions
 * for deadletter handlers.
 *
 * This object will be shared between all agents.
 */
const state_t deadletter_state(
		nullptr, "<DEADLETTER_STATE>" );

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

} /* namespace anonymous */

bool
state_t::is_target( const agent_t * agent ) const noexcept
{
	if( m_target_agent )
		return m_target_agent == agent;
	else if( this == &awaiting_deregistration_state )
		return true;
	else
		return false;
}

void
state_t::activate() const
{
	m_target_agent->so_change_state( *this );
}

state_t &
state_t::time_limit(
	duration_t timeout,
	const state_t & state_to_switch )
{
	if( duration_t::zero() == timeout )
		SO_5_THROW_EXCEPTION( rc_invalid_time_limit_for_state,
				"zero can't be used as time limit for state: " +
				query_name() );

	// Old time limit must be dropped if it exists.
	{
		// As a defense from exception create new time_limit object first.
		auto fresh_limit = std::make_unique< time_limit_t >(
				timeout, std::cref(state_to_switch) );
		drop_time_limit();
		m_time_limit = std::move(fresh_limit);
	}

	// If this state is active then new time limit must be activated.
	if( is_active() )
		so_5::details::do_with_rollback_on_exception(
			[&] {
				m_time_limit->set_up_limit_for_agent( *m_target_agent, *this );
			},
			[&] {
				// Time limit must be dropped because it is not activated
				// for the current state.
				drop_time_limit();
			} );

	return *this;
}

state_t &
state_t::drop_time_limit()
{
	if( m_time_limit )
	{
		m_time_limit->drop_limit_for_agent( *m_target_agent, *this );
		m_time_limit.reset();
	}

	return *this;
}

const state_t *
state_t::actual_state_to_enter() const
{
	const state_t * s = this;
	while( 0 != s->m_substate_count )
	{
		if( s->m_last_active_substate )
			// Note: for states with shallow history m_last_active_substate
			// can point to composite substate. This substate must be
			// processed usual way with checking for substate count, presence
			// of initial substate and so on...
			s = s->m_last_active_substate;
		else if( !s->m_initial_substate )
			SO_5_THROW_EXCEPTION( rc_no_initial_substate,
					"there is no initial substate for composite state: " +
					query_name() );
		else
			s = s->m_initial_substate;
	}

	return s;
}

void
state_t::update_history_in_parent_states() const
{
	auto p = m_parent_state;

	// This pointer will be used for update states with shallow history.
	// This pointer will be changed on every iteration.
	auto c = this;

	while( p )
	{
		if( history_t::shallow == p->m_state_history )
			p->m_last_active_substate = c;
		else if( history_t::deep == p->m_state_history )
			p->m_last_active_substate = this;

		c = p;
		p = p->m_parent_state;
	}
}

void
state_t::handle_time_limit_on_enter() const
{
	m_time_limit->set_up_limit_for_agent( *m_target_agent, *this );
}

void
state_t::handle_time_limit_on_exit() const
{
	m_time_limit->drop_limit_for_agent( *m_target_agent, *this );
}

namespace
{

/*!
 * \brief Helper for creation of the direct mbox for an agent.
 *
 * If there is a custom direct mbox factory in \a tuning_options
 * then the return value of that factory is used. Otherwise the
 * \a standard_mbox is returned.
 */
[[nodiscard]]
mbox_t
make_direct_mbox_with_respect_to_custom_factory(
	partially_constructed_agent_ptr_t agent_ptr,
	const agent_tuning_options_t & tuning_options,
	mbox_t standard_mbox )
	{
		mbox_t result{ std::move(standard_mbox) };

		const auto & factory =
				tuning_options.query_custom_direct_mbox_factory();
		if( factory )
			{
				result = factory( agent_ptr, std::move(result) );

				if( mbox_type_t::multi_producer_single_consumer
						!= result->type() )
					{
						SO_5_THROW_EXCEPTION(
								rc_mpsc_mbox_expected,
								"MPSC mbox is expected as the direct mbox "
								"for an agent" );
					}
			}

		return result;
	}

/*!
 * \brief Helper for selection of subscription storage factory.
 *
 * If a factory is specified in @a tuning_options explicitly then it's
 * used. Otherwise the default subscription storage factory from
 * SObjectizer Environment will be used.
 *
 * \since v.5.8.2
 */
[[nodiscard]]
subscription_storage_factory_t
detect_subscription_storage_factory_to_use(
	environment_t & env,
	const agent_tuning_options_t & tuning_options )
	{
		if( tuning_options.is_user_provided_subscription_storage_factory() )
			return tuning_options.query_subscription_storage_factory();
		else
			return impl::internal_env_iface_t{ env }.default_subscription_storage_factory();
	}

} /* namespace anonymous */

//
// agent_t
//

agent_t::agent_t(
	environment_t & env )
	:	agent_t( env, tuning_options() )
{
}

agent_t::agent_t(
	environment_t & env,
	agent_tuning_options_t options )
	:	agent_t( context_t( env, std::move( options ) ) )
{
}

agent_t::agent_t(
	context_t ctx )
	:	m_current_state_ptr( &st_default )
	,	m_current_status( agent_status_t::not_defined_yet )
	,	m_handler_finder(
			// Actual handler finder is dependent on msg_tracing status.
			impl::internal_env_iface_t( ctx.env() ).is_msg_tracing_enabled() ?
				&agent_t::handler_finder_msg_tracing_enabled :
				&agent_t::handler_finder_msg_tracing_disabled )
	,	m_subscriptions(
			detect_subscription_storage_factory_to_use( ctx.env(), ctx.options() )() )
	,	m_message_sinks(
			impl::create_sinks_storage_if_necessary(
				partially_constructed_agent_ptr_t( self_ptr() ),
				ctx.options().giveout_message_limits() ) )
	,	m_env( ctx.env() )
	,	m_event_queue( nullptr )
	,	m_direct_mbox(
			make_direct_mbox_with_respect_to_custom_factory(
				partially_constructed_agent_ptr_t( self_ptr() ),
				ctx.options(),
				impl::internal_env_iface_t( ctx.env() ).create_ordinary_mpsc_mbox(
						*self_ptr() )
			)
		)
		// It is necessary to enable agent subscription in the
		// constructor of derived class.
	,	m_working_thread_id( so_5::query_current_thread_id() )
	,	m_agent_coop( nullptr )
	,	m_priority( ctx.options().query_priority() )
	,	m_name( ctx.options().giveout_agent_name() )
{
}

agent_t::~agent_t()
{
	// Sometimes it is possible that agent is destroyed without
	// correct deregistration from SO Environment.
	destroy_all_subscriptions_and_filters();
}

void
agent_t::so_evt_start()
{
	// Default implementation do nothing.
}

void
agent_t::so_evt_finish()
{
	// Default implementation do nothing.
}

bool
agent_t::so_is_active_state( const state_t & state_to_check ) const noexcept
{
	state_t::path_t path;
	m_current_state_ptr->fill_path( path );

	const auto past_the_end = [&path, this]() {
			auto r = begin(path);
			std::advance( r, m_current_state_ptr->nested_level() + 1u );
			return r;
		}();

	return past_the_end != std::find( begin(path), past_the_end, &state_to_check );
}

void
agent_t::so_add_nondestroyable_listener(
	agent_state_listener_t & state_listener )
{
	m_state_listener_controller.add(
			impl::state_listener_controller_t::wrap_nondestroyable(
					state_listener ) );
}

void
agent_t::so_add_destroyable_listener(
	agent_state_listener_unique_ptr_t state_listener )
{
	m_state_listener_controller.add(
			impl::state_listener_controller_t::wrap_destroyable(
					std::move( state_listener ) ) );
}

exception_reaction_t
agent_t::so_exception_reaction() const noexcept
{
	if( m_agent_coop )
		return m_agent_coop->exception_reaction();
	else
		// This is very strange case. So it would be better to abort.
		return abort_on_exception;
}

void
agent_t::so_switch_to_awaiting_deregistration_state()
{
	so_deactivate_agent();
}

const mbox_t &
agent_t::so_direct_mbox() const
{
	return m_direct_mbox;
}

mbox_t
agent_t::so_make_new_direct_mbox()
{
	return impl::internal_env_iface_t{ so_environment() }.create_ordinary_mpsc_mbox(
			*self_ptr() );
}

const state_t &
agent_t::so_default_state() const
{
	return st_default;
}

namespace impl {

class state_switch_guard_t
{
	agent_t & m_agent;
	agent_t::agent_status_t m_previous_status;

public :
	state_switch_guard_t(
		agent_t & agent )
		:	m_agent( agent )
		,	m_previous_status( agent.m_current_status )
	{
		if( agent_t::agent_status_t::state_switch_in_progress
				== agent.m_current_status ) 
			SO_5_THROW_EXCEPTION(
					rc_another_state_switch_in_progress,
					"an attempt to switch agent state when another state "
					"switch operation is in progress for the same agent" );

		agent.m_current_status = agent_t::agent_status_t::state_switch_in_progress;
	}
	~state_switch_guard_t()
	{
		m_agent.m_current_status = m_previous_status;
	}
};

} /* namespace impl */

void
agent_t::so_change_state(
	const state_t & new_state )
{
	ensure_operation_is_on_working_thread( "so_change_state" );

	do_change_agent_state( new_state );
}

void
agent_t::so_deactivate_agent()
{
	ensure_operation_is_on_working_thread( "so_deactivate_agent" );

	do_change_agent_state( awaiting_deregistration_state );
	destroy_all_subscriptions_and_filters();
}

void
agent_t::so_initiate_agent_definition()
{
	impl::agent_impl::working_thread_id_sentinel_t sentinel(
			m_working_thread_id,
			so_5::query_current_thread_id() );

	so_define_agent();

	m_current_status = agent_status_t::defined;
}

void
agent_t::so_define_agent()
{
	// Default implementation do nothing.
}

bool
agent_t::so_was_defined() const
{
	return agent_status_t::not_defined_yet != m_current_status;
}

environment_t &
agent_t::so_environment() const noexcept
{
	return m_env;
}

[[nodiscard]]
coop_handle_t
agent_t::so_coop() const
{
	if( !m_agent_coop )
		SO_5_THROW_EXCEPTION(
				rc_agent_has_no_cooperation,
				"agent_t::so_coop() can be completed because agent is not bound "
				"to any cooperation" );

	return m_agent_coop->handle();
}

void
agent_t::so_bind_to_dispatcher(
	event_queue_t & queue ) noexcept
{
	// Since v.5.5.24 we should use event_queue_hook to get an
	// actual event_queue.
	auto * actual_queue = impl::internal_env_iface_t{ m_env }
			.event_queue_on_bind( this, &queue );

	std::lock_guard< default_rw_spinlock_t > queue_lock{ m_event_queue_lock };

	// Cooperation usage counter should be incremented.
	// It will be decremented during final agent event execution.
	impl::coop_private_iface_t::increment_usage_count( *m_agent_coop );

	// A starting demand must be sent first.
	actual_queue->push_evt_start(
			execution_demand_t(
					this,
					message_limit::control_block_t::none(),
					0,
					typeid(void),
					message_ref_t(),
					&agent_t::demand_handler_on_start ) );
	
	// Only then pointer to the queue could be stored.
	m_event_queue = actual_queue;
}

execution_hint_t
agent_t::so_create_execution_hint(
	execution_demand_t & d )
{
	enum class demand_type_t {
			message, enveloped_msg, other
	};

	// We can't use message_kind_t here because there are special
	// demands like demands for so_evt_start/so_evt_finish.
	// Because of that a pointer to demand handler will be analyzed.
	const auto demand_type =
			(d.m_demand_handler == &agent_t::demand_handler_on_message ?
				demand_type_t::message :
				(d.m_demand_handler == &agent_t::demand_handler_on_enveloped_msg ?
					demand_type_t::enveloped_msg : demand_type_t::other));

	if( demand_type_t::other != demand_type )
		{
			// Try to find handler for the demand.
			auto handler = d.m_receiver->m_handler_finder(
					d, "create_execution_hint" );
			if( demand_type_t::message == demand_type )
				{
					if( handler )
						return execution_hint_t(
								d,
								[handler](
										execution_demand_t & demand,
										current_thread_id_t thread_id ) {
									process_message(
											thread_id,
											demand,
											handler->m_thread_safety,
											handler->m_method );
								},
								handler->m_thread_safety );
					else
						// Handler not found.
						return execution_hint_t::create_empty_execution_hint( d );
				}
			else
				{
					// Execution hint for enveloped message is
					// very similar to hint for service request.
					return execution_hint_t(
							d,
							[handler](
									execution_demand_t & demand,
									current_thread_id_t thread_id ) {
								process_enveloped_msg(
										thread_id,
										demand,
										handler );
							},
							handler ? handler->m_thread_safety :
								// If there is no real handler then
								// there will only be actions from
								// envelope.
								// These actions should be thread safe.
								thread_safe );
				}
		}
	else
		// This is demand_handler_on_start or demand_handler_on_finish.
		return execution_hint_t(
				d,
				[]( execution_demand_t & demand,
					current_thread_id_t thread_id ) {
					demand.call_handler( thread_id );
				},
				not_thread_safe );
}

void
agent_t::so_deregister_agent_coop( int dereg_reason )
{
	so_environment().deregister_coop(
			m_agent_coop->handle(), dereg_reason );
}

void
agent_t::so_deregister_agent_coop_normally()
{
	so_deregister_agent_coop( dereg_reason::normal );
}

disp_binder_shptr_t
agent_t::so_this_coop_disp_binder() const
{
	if( !m_agent_coop )
		SO_5_THROW_EXCEPTION(
				rc_agent_has_no_cooperation,
				"agent_t::so_this_coop_disp_binder() can be completed "
				"because agent is not bound to any cooperation" );

	return m_agent_coop->coop_disp_binder();
}

agent_identity_t
agent_t::so_agent_name() const noexcept
{
	if( m_name.has_value() )
		return { m_name.as_string_view() };
	else
		return { this };
}

void
agent_t::destroy_all_subscriptions_and_filters() noexcept
{
	drop_all_delivery_filters();
	m_subscriptions->drop_all_subscriptions();
}

agent_ref_t
agent_t::create_ref()
{
	agent_ref_t agent_ref( this );
	return agent_ref;
}

void
agent_t::bind_to_coop( coop_t & coop )
{
	m_agent_coop = &coop;
}

void
agent_t::shutdown_agent() noexcept
{
	event_queue_t * actual_queue = nullptr;
	{
		std::lock_guard< default_rw_spinlock_t > queue_lock{ m_event_queue_lock };

		// Since v.5.5.8 shutdown is done by two simple step:
		// - remove actual value from m_event_queue;
		// - pushing final demand to actual event queue.
		// 
		// No new demands will be sent to the agent, but all the subscriptions
		// remains. They will be destroyed at the very end of agent's lifetime.

		if( m_event_queue )
		{
			// This pointer will be used later.
			actual_queue = m_event_queue;

			// Final event must be pushed to queue.
			so_5::details::invoke_noexcept_code( [&] {
					m_event_queue->push_evt_finish(
							execution_demand_t(
									this,
									message_limit::control_block_t::none(),
									0,
									typeid(void),
									message_ref_t(),
									&agent_t::demand_handler_on_finish ) );

					// No more events will be stored to the queue.
					m_event_queue = nullptr;
				} );

		}
		else
			so_5::details::abort_on_fatal_error( [&] {
				SO_5_LOG_ERROR( so_environment(), log_stream )
				{
					log_stream << "Unexpected error: m_event_queue contains "
						"nullptr. Unable to push demand_handler_on_finish for "
						"the agent (" << this << "). Application will be aborted"
						<< std::endl;
				}
			} );
	}

	if( actual_queue )
		// Since v.5.5.24 we should utilize event_queue via
		// event_queue_hook.
		impl::internal_env_iface_t{ m_env }
				.event_queue_on_unbind( this, actual_queue );
}

void
agent_t::so_create_event_subscription(
	const mbox_t & mbox_ref,
	std::type_index msg_type,
	const state_t & target_state,
	const event_handler_method_t & method,
	thread_safety_t thread_safety,
	event_handler_kind_t handler_kind )
{
	// Since v.5.4.0 there is no need for locking agent's mutex
	// because this operation can be performed only on agent's
	// working thread.

	ensure_operation_is_on_working_thread( "so_create_event_subscription" );

	// A new subscription can't be made if agent is already deactivated.
	if( is_agent_deactivated() )
		SO_5_THROW_EXCEPTION(
				so_5::rc_agent_deactivated,
				"new subscription can't made for deactivated agent" );

	m_subscriptions->create_event_subscription(
			mbox_ref,
			msg_type,
			detect_sink_for_message_type( msg_type ),
			target_state,
			method,
			thread_safety,
			handler_kind );
}

void
agent_t::so_create_deadletter_subscription(
	const mbox_t & mbox,
	const std::type_index & msg_type,
	const event_handler_method_t & method,
	thread_safety_t thread_safety )
{
	ensure_operation_is_on_working_thread( "so_create_deadletter_subscription" );

	// A new deadletter handler can't be made if agent is already deactivated.
	if( is_agent_deactivated() )
		SO_5_THROW_EXCEPTION(
				so_5::rc_agent_deactivated,
				"new deadletter handler can't be set for deactivated agent" );

	m_subscriptions->create_event_subscription(
			mbox,
			msg_type,
			detect_sink_for_message_type( msg_type ),
			deadletter_state,
			method,
			thread_safety,
			event_handler_kind_t::final_handler );
}

void
agent_t::so_destroy_deadletter_subscription(
	const mbox_t & mbox,
	const std::type_index & msg_type )
{
	// Since v.5.4.0 there is no need for locking agent's mutex
	// because this operation can be performed only on agent's
	// working thread.

	ensure_operation_is_on_working_thread( "do_drop_deadletter_handler" );

	m_subscriptions->drop_subscription( mbox, msg_type, deadletter_state );
}

abstract_message_sink_t &
agent_t::detect_sink_for_message_type(
	const std::type_index & msg_type )
{
	auto * result = m_message_sinks->find_or_create( msg_type );

	if( !result )
		SO_5_THROW_EXCEPTION(
				so_5::rc_message_has_no_limit_defined,
				std::string( "message type without "
				"predefined limit for that type, type: " ) +
				msg_type.name() );

	return *result;
}

void
agent_t::do_drop_subscription(
	const mbox_t & mbox,
	const std::type_index & msg_type,
	const state_t & target_state )
{
	// Since v.5.4.0 there is no need for locking agent's mutex
	// because this operation can be performed only on agent's
	// working thread.

	ensure_operation_is_on_working_thread( "do_drop_subscription" );

	m_subscriptions->drop_subscription( mbox, msg_type, target_state );
}

void
agent_t::do_drop_subscription_for_all_states(
	const mbox_t & mbox,
	const std::type_index & msg_type )
{
	// Since v.5.4.0 there is no need for locking agent's mutex
	// because this operation can be performed only on agent's
	// working thread.

	ensure_operation_is_on_working_thread(
			"do_drop_subscription_for_all_states" );

	m_subscriptions->drop_subscription_for_all_states( mbox, msg_type );
}

bool
agent_t::do_check_subscription_presence(
	const mbox_t & mbox,
	const std::type_index & msg_type,
	const state_t & target_state ) const noexcept
{
	return nullptr != m_subscriptions->find_handler(
			mbox->id(), msg_type, target_state );
}

bool
agent_t::do_check_deadletter_presence(
	const mbox_t & mbox,
	const std::type_index & msg_type ) const noexcept
{
	return nullptr != m_subscriptions->find_handler(
			mbox->id(), msg_type, deadletter_state );
}

namespace {

/*!
 * \brief A helper function to select actual demand handler in
 * dependency of message kind.
 *
 * \since
 * v.5.5.23
 */
inline demand_handler_pfn_t
select_demand_handler_for_message(
	const agent_t & agent,
	const message_ref_t & msg )
{
	demand_handler_pfn_t result = &agent_t::demand_handler_on_message;
	if( msg )
	{
		switch( message_kind( *msg ) )
		{
		case message_t::kind_t::classical_message : // Already has value.
		break;

		case message_t::kind_t::user_type_message : // Already has value.
		break;

		case message_t::kind_t::enveloped_msg :
			result = &agent_t::demand_handler_on_enveloped_msg;
		break;

		case message_t::kind_t::signal :
			so_5::details::abort_on_fatal_error( [&] {
				SO_5_LOG_ERROR( agent.so_environment(), log_stream )
				{
					log_stream << "message that has data and message_kind_t::signal!"
							"Signals can't have data. Application will be aborted!"
							<< std::endl;
				}
			} );
		break;
		}
	}

	return result;
}

} /* namespace anonymous */

void
agent_t::push_event(
	const message_limit::control_block_t * limit,
	mbox_id_t mbox_id,
	const std::type_index & msg_type,
	const message_ref_t & message )
{
	const auto handler = select_demand_handler_for_message( *this, message );

	read_lock_guard_t< default_rw_spinlock_t > queue_lock{ m_event_queue_lock };

	if( m_event_queue )
		m_event_queue->push(
				execution_demand_t(
					this,
					limit,
					mbox_id,
					msg_type,
					message,
					handler ) );
}

void
agent_t::demand_handler_on_start(
	current_thread_id_t working_thread_id,
	execution_demand_t & d )
{
	d.m_receiver->ensure_binding_finished();

	impl::agent_impl::working_thread_id_sentinel_t sentinel(
			d.m_receiver->m_working_thread_id,
			working_thread_id );

	try
	{
		d.m_receiver->so_evt_start();
	}
	catch( const std::exception & x )
	{
		impl::process_unhandled_exception(
				working_thread_id, x, *(d.m_receiver) );
	}
	catch( ... ) // Since v.5.5.24.3
	{
		impl::process_unhandled_unknown_exception(
				working_thread_id, *(d.m_receiver) );
	}
}

void
agent_t::ensure_binding_finished()
{
	// Nothing more to do.
	// Just lock coop's binding_lock. If cooperation is not finished yet
	// it would stop the current thread.
	std::lock_guard< std::mutex > binding_lock{ m_agent_coop->m_lock };
}

demand_handler_pfn_t
agent_t::get_demand_handler_on_start_ptr() noexcept
{
	return &agent_t::demand_handler_on_start;
}

void
agent_t::demand_handler_on_finish(
	current_thread_id_t working_thread_id,
	execution_demand_t & d )
{
	{
		// Sentinel must finish its work before decrementing
		// reference count to cooperation.
		impl::agent_impl::working_thread_id_sentinel_t sentinel(
				d.m_receiver->m_working_thread_id,
				working_thread_id );

		try
		{
			d.m_receiver->so_evt_finish();
		}
		catch( const std::exception & x )
		{
			impl::process_unhandled_exception(
					working_thread_id, x, *(d.m_receiver) );
		}
		catch( ... ) // Since v.5.5.24.3
		{
			impl::process_unhandled_unknown_exception(
					working_thread_id, *(d.m_receiver) );
		}

		// Since v.5.5.15 agent should be returned in default state.
		d.m_receiver->return_to_default_state_if_possible();
	}

	// Cooperation should receive notification about agent deregistration.
	impl::coop_private_iface_t::decrement_usage_count(
			*(d.m_receiver->m_agent_coop) );
}

demand_handler_pfn_t
agent_t::get_demand_handler_on_finish_ptr() noexcept
{
	return &agent_t::demand_handler_on_finish;
}

void
agent_t::demand_handler_on_message(
	current_thread_id_t working_thread_id,
	execution_demand_t & d )
{
	message_limit::control_block_t::decrement( d.m_limit );

	auto handler = d.m_receiver->m_handler_finder(
			d, "demand_handler_on_message" );
	if( handler )
		process_message(
				working_thread_id,
				d,
				handler->m_thread_safety,
				handler->m_method );
}

demand_handler_pfn_t
agent_t::get_demand_handler_on_message_ptr() noexcept
{
	return &agent_t::demand_handler_on_message;
}

void
agent_t::demand_handler_on_enveloped_msg(
	current_thread_id_t working_thread_id,
	execution_demand_t & d )
{
	message_limit::control_block_t::decrement( d.m_limit );

	auto handler = d.m_receiver->m_handler_finder(
			d, "demand_handler_on_enveloped_msg" );
	process_enveloped_msg( working_thread_id, d, handler );
}

demand_handler_pfn_t
agent_t::get_demand_handler_on_enveloped_msg_ptr() noexcept
{
	return &agent_t::demand_handler_on_enveloped_msg;
}

void
agent_t::process_message(
	current_thread_id_t working_thread_id,
	execution_demand_t & d,
	thread_safety_t thread_safety,
	event_handler_method_t method )
{
	impl::agent_impl::working_thread_id_sentinel_t sentinel{
			d.m_receiver->m_working_thread_id,
			// v.5.7.3
			// If event_handler is thread_safe-handler then null_thread_id
			// has to be used instead of the actual working_thread_id.
			so_5::thread_safe == thread_safety
				? null_current_thread_id()
				: working_thread_id
		};

	try
	{
		method( d.m_message_ref );
	}
	catch( const std::exception & x )
	{
		impl::process_unhandled_exception(
				working_thread_id, x, *(d.m_receiver) );
	}
	catch( ... ) // Since v.5.5.24.3
	{
		impl::process_unhandled_unknown_exception(
				working_thread_id, *(d.m_receiver) );
	}
}

void
agent_t::process_enveloped_msg(
	current_thread_id_t working_thread_id,
	execution_demand_t & d,
	const impl::event_handler_data_t * handler_data )
{
	using namespace enveloped_msg::impl;

	if( handler_data )
	{
		// If this is intermediate_handler then we should pass the
		// whole envelope to it.
		if( event_handler_kind_t::intermediate_handler == handler_data->m_kind )
			// Just call process_message() in that case because
			// process_message() already does what we need (including
			// setting working_thread_id and handling of exceptions).
			process_message(
					working_thread_id,
					d,
					handler_data->m_thread_safety,
					handler_data->m_method );
		else
			// For a final_handler the payload should be extracted
			// from the envelope and the extracted payload should go
			// to the handler.
			// We don't expect exceptions here and can't restore after them.
			so_5::details::invoke_noexcept_code( [&] {
				auto & envelope = message_to_envelope( d.m_message_ref );
				agent_demand_handler_invoker_t invoker{
						working_thread_id,
						d,
						*handler_data
				};
				envelope.access_hook(
						so_5::enveloped_msg::access_context_t::handler_found,
						invoker );
			} );
	}
}

void
agent_t::ensure_operation_is_on_working_thread(
	const char * operation_name ) const
{
	if( so_5::query_current_thread_id() != m_working_thread_id )
	{
		std::ostringstream s;

		s << operation_name
			<< ": operation is enabled only on agent's working thread; "
			<< "working_thread_id: ";

		if( m_working_thread_id == null_current_thread_id() )
			s << "<NONE>";
		else
			s << m_working_thread_id;

		s << ", current_thread_id: " << so_5::query_current_thread_id();

		SO_5_THROW_EXCEPTION(
				so_5::rc_operation_enabled_only_on_agent_working_thread,
				s.str() );
	}
}

void
agent_t::drop_all_delivery_filters() noexcept
{
	if( m_delivery_filters )
	{
		m_delivery_filters->drop_all();
		m_delivery_filters.reset();
	}
}

void
agent_t::do_set_delivery_filter(
	const mbox_t & mbox,
	const std::type_index & msg_type,
	delivery_filter_unique_ptr_t filter )
{
	ensure_operation_is_on_working_thread( "set_delivery_filter" );

	// A new delivery filter can't be set if agent is already deactivated.
	if( is_agent_deactivated() )
		SO_5_THROW_EXCEPTION(
				so_5::rc_agent_deactivated,
				"new delivery filter can't be set for deactivated agent" );

	// Message sink for that message type have to be obtained with
	// the respect to message limits.
	auto & target_sink = detect_sink_for_message_type( msg_type );

	if( !m_delivery_filters )
		m_delivery_filters.reset( new impl::delivery_filter_storage_t() );

	m_delivery_filters->set_delivery_filter(
			mbox,
			msg_type,
			std::move(filter),
			outliving_mutable( target_sink ) );
}

void
agent_t::do_drop_delivery_filter(
	const mbox_t & mbox,
	const std::type_index & msg_type ) noexcept
{
	ensure_operation_is_on_working_thread( "set_delivery_filter" );

	if( m_delivery_filters )
		m_delivery_filters->drop_delivery_filter( mbox, msg_type );
}

const impl::event_handler_data_t *
agent_t::handler_finder_msg_tracing_disabled(
	execution_demand_t & d,
	const char * /*context_marker*/ )
{
	auto search_result = find_event_handler_for_current_state( d );
	if( !search_result )
		// Since v.5.5.21 we should check for deadletter handler for that demand.
		search_result = find_deadletter_handler( d );

	return search_result;
}

const impl::event_handler_data_t *
agent_t::handler_finder_msg_tracing_enabled(
	execution_demand_t & d,
	const char * context_marker )
{
	auto search_result = find_event_handler_for_current_state( d );

	if( !search_result )
	{
		// Since v.5.5.21 we should check for deadletter handler for that demand.
		search_result = find_deadletter_handler( d );

		if( search_result )
		{
			// Deadletter handler found. This must be reflected in trace.
			impl::msg_tracing_helpers::trace_deadletter_handler_search_result(
					d,
					context_marker,
					search_result );

			return search_result;
		}
	}

	// This trace will be made if an event_handler is found for the
	// current state or not found at all (including deadletter handlers).
	impl::msg_tracing_helpers::trace_event_handler_search_result(
			d,
			context_marker,
			search_result );

	return search_result;
}

const impl::event_handler_data_t *
agent_t::find_event_handler_for_current_state(
	execution_demand_t & d )
{
	const impl::event_handler_data_t * search_result = nullptr;
	const state_t * s = &d.m_receiver->so_current_state();

	do {
		search_result = d.m_receiver->m_subscriptions->find_handler(
				d.m_mbox_id,
				d.m_msg_type, 
				*s );

		if( !search_result )
			s = s->parent_state();

	} while( search_result == nullptr && s != nullptr );

	return search_result;
}

const impl::event_handler_data_t *
agent_t::find_deadletter_handler(
	execution_demand_t & demand )
{
	return demand.m_receiver->m_subscriptions->find_handler(
			demand.m_mbox_id,
			demand.m_msg_type,
			deadletter_state );
}

void
agent_t::do_change_agent_state(
	const state_t & state_to_be_set )
{
	// The agent can't leave awaiting_deregistration_state if it's
	// in that state already.
	if( is_agent_deactivated() &&
			state_to_be_set != awaiting_deregistration_state )
	{
		SO_5_THROW_EXCEPTION(
			rc_agent_deactivated,
			"unable to switch agent to another state because the "
			"agent is already deactivated" );
	}

	if( state_to_be_set.is_target( this ) )
	{
		// Since v.5.5.18 we must check nested state switch operations.
		// This object will drop pointer to the current state.
		impl::state_switch_guard_t switch_op_guard( *this );

		auto actual_new_state = state_to_be_set.actual_state_to_enter();
		if( !( *actual_new_state == *m_current_state_ptr ) )
		{
			// New state differs from the current one.
			// Actual state switch must be performed.
			do_state_switch( *actual_new_state );

			// State listener should be informed.
			m_state_listener_controller.changed(
				*this,
				*m_current_state_ptr );
		}
	}
	else
		SO_5_THROW_EXCEPTION(
			rc_agent_unknown_state,
			"unable to switch agent to alien state "
			"(the state that doesn't belong to this agent)" );
}

void
agent_t::do_state_switch(
	const state_t & state_to_be_set ) noexcept
{
	state_t::path_t old_path;
	state_t::path_t new_path;

	// Since v.5.5.22 we will change the value of m_current_state_ptr
	// during state change procedure.
	auto current_st = m_current_state_ptr;

	current_st->fill_path( old_path );
	state_to_be_set.fill_path( new_path );

	// Find the first item which is different in the paths.
	std::size_t first_diff = 0;
	for(; first_diff < std::min(
				current_st->nested_level(),
				state_to_be_set.nested_level() );
			++first_diff )
		if( old_path[ first_diff ] != new_path[ first_diff ] )
			break;

	// Do call for on_exit and on_enter for states.
	// on_exit and on_enter should not throw exceptions.
	so_5::details::invoke_noexcept_code( [&] {

		impl::msg_tracing_helpers::safe_trace_state_leaving(
				*this, *current_st );

		for( std::size_t i = current_st->nested_level();
				i >= first_diff; )
			{
				// Modify current state before calling on_exit handler.
				m_current_state_ptr = old_path[ i ];
				// Perform on_exit actions.
				old_path[ i ]->call_on_exit();
				if( i )
					--i;
				else
					break;
			}

		impl::msg_tracing_helpers::safe_trace_state_entering(
				*this, state_to_be_set );

		for( std::size_t i = first_diff;
				i <= state_to_be_set.nested_level();
				++i )
			{
				// Modify current state before calling on_exit handler.
				m_current_state_ptr = new_path[ i ];

				// Perform on_enter actions.
				new_path[ i ]->call_on_enter();
			}
	} );

	// Now the current state for the agent can be changed.
	m_current_state_ptr = &state_to_be_set;
	m_current_state_ptr->update_history_in_parent_states();
}

void
agent_t::return_to_default_state_if_possible() noexcept
{
	if( !( st_default == so_current_state() ||
			is_agent_deactivated() ) )
	{
		// The agent must be returned to the default state.
		// All on_exit handlers must be called at this point.
		so_change_state( st_default );
	}
}

bool
agent_t::is_agent_deactivated() const noexcept
{
	return awaiting_deregistration_state == so_current_state();
}

} /* namespace so_5 */

