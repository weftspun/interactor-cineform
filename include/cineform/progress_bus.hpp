// The progress service, both ends.
//
// One file for the publisher and the subscriber, because they have to agree on four things
// the bus checks at connect time -- service name, payload type name, size and alignment -- and
// iceoryx2 refuses to connect two services that disagree on any of them. Two copies of those
// four values is exactly the drift `weft/command.hpp` exists to prevent one layer up, so this
// is the same arrangement: the shape lives in one header and both sides include it.
//
// Every iox2_ call here goes through the harness's dlsym table. Nothing links iceoryx2.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT
#ifndef CINEFORM_PROGRESS_BUS_HPP
#define CINEFORM_PROGRESS_BUS_HPP

#include "cineform/wire.hpp"

#include "iox2_api.h"

#include <cstdio>
#include <cstring>

namespace cineform {

namespace detail {

// The FIXED_SIZE service both ends open. `open_or_create` rather than `create`, so neither
// end has to start first -- a display launched before its encoder is an ordinary case.
inline iox2_port_factory_pub_sub_h open_progress_service(iox2_node_h *node,
		iox2_service_name_h *name_out) {
	iox2_service_name_h name = nullptr;
	if (iox2_service_name_new(nullptr, PROGRESS_SERVICE_NAME, std::strlen(PROGRESS_SERVICE_NAME),
				&name) != IOX2_OK) {
		return nullptr;
	}
	auto builder = iox2_service_builder_pub_sub(
			iox2_node_service_builder(node, nullptr, iox2_cast_service_name_ptr(name)));
	if (iox2_service_builder_pub_sub_set_payload_type_details(&builder,
				iox2_type_variant_e_FIXED_SIZE, PROGRESS_TYPE, std::strlen(PROGRESS_TYPE),
				sizeof(Progress), alignof(Progress)) != IOX2_OK) {
		iox2_service_name_drop(name);
		return nullptr;
	}
	iox2_port_factory_pub_sub_h service = nullptr;
	if (iox2_service_builder_pub_sub_open_or_create(builder, nullptr, &service) != IOX2_OK) {
		iox2_service_name_drop(name);
		return nullptr;
	}
	*name_out = name;
	return service;
}

} // namespace detail

// The encoder's end. Publishing is fire and forget: a failed send costs one status line and
// must never stop or slow an encode, so nothing here returns a value the caller has to act on.
class ProgressPublisher {
public:
	bool open() {
		if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
					iox2_service_type_e_IPC, &node_) != IOX2_OK) {
			std::fprintf(stderr, "cineform: progress publisher has no node\n");
			return false;
		}
		service_ = detail::open_progress_service(&node_, &name_);
		if (service_ == nullptr) {
			std::fprintf(stderr, "cineform: could not open %s\n", PROGRESS_SERVICE_NAME);
			return false;
		}
		if (iox2_port_factory_publisher_builder_create(
					iox2_port_factory_pub_sub_publisher_builder(&service_, nullptr), nullptr,
					&publisher_) != IOX2_OK) {
			std::fprintf(stderr, "cineform: no progress publisher\n");
			return false;
		}
		return true;
	}

	void send(const Progress &p) {
		if (publisher_ == nullptr) {
			return;
		}
		iox2_sample_mut_h sample = nullptr;
		if (iox2_publisher_loan_slice_uninit(&publisher_, nullptr, &sample, 1) != IOX2_OK) {
			// No loan means every subscriber is behind and holding the whole buffer. Dropping
			// this sample is the design: see PROGRESS_BUFFER in wire.hpp.
			return;
		}
		void *payload = nullptr;
		size_t elements = 0;
		iox2_sample_mut_payload_mut(&sample, &payload, &elements);
		std::memcpy(payload, &p, sizeof(Progress));
		(void)iox2_sample_mut_send(sample, nullptr);
	}

	void close() {
		if (publisher_) { iox2_publisher_drop(publisher_); publisher_ = nullptr; }
		if (service_) { iox2_port_factory_pub_sub_drop(service_); service_ = nullptr; }
		if (name_) { iox2_service_name_drop(name_); name_ = nullptr; }
		if (node_) { iox2_node_drop(node_); node_ = nullptr; }
	}

	~ProgressPublisher() { close(); }

private:
	iox2_node_h node_ = nullptr;
	iox2_service_name_h name_ = nullptr;
	iox2_port_factory_pub_sub_h service_ = nullptr;
	iox2_publisher_h publisher_ = nullptr;
};

// The display's end.
class ProgressSubscriber {
public:
	bool open() {
		if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
					iox2_service_type_e_IPC, &node_) != IOX2_OK) {
			std::fprintf(stderr, "cineform: progress subscriber has no node\n");
			return false;
		}
		service_ = detail::open_progress_service(&node_, &name_);
		if (service_ == nullptr) {
			std::fprintf(stderr, "cineform: could not open %s\n", PROGRESS_SERVICE_NAME);
			return false;
		}
		// The buffer depth is iceoryx2's default and is NOT set here. See PROGRESS_BUFFER in
		// wire.hpp: the setter is not in the harness's generated symbol table, and adding it
		// would mean extending that table unverified in the same change that introduces this
		// service.
		auto builder = iox2_port_factory_pub_sub_subscriber_builder(&service_, nullptr);
		if (iox2_port_factory_subscriber_builder_create(builder, nullptr, &subscriber_) != IOX2_OK) {
			std::fprintf(stderr, "cineform: no progress subscriber\n");
			return false;
		}
		return true;
	}

	// Takes the NEWEST sample waiting and discards the rest, because a status line showing a
	// frame number from two seconds ago is worse than one that skipped to now. Returns false
	// when nothing was waiting.
	bool latest(Progress *out) {
		bool got = false;
		for (;;) {
			iox2_sample_h sample = nullptr;
			if (iox2_subscriber_receive(&subscriber_, nullptr, &sample) != IOX2_OK) {
				break;
			}
			if (!sample) {
				break;
			}
			const void *payload = nullptr;
			size_t elements = 0;
			iox2_sample_payload(&sample, &payload, &elements);
			std::memcpy(out, payload, sizeof(Progress));
			iox2_sample_drop(sample);
			got = true;
		}
		return got;
	}

	// Waits `ns` for something to arrive, so a display idles instead of spinning a core.
	void wait(uint32_t ns) { (void)iox2_node_wait(&node_, 0, ns); }

	void close() {
		if (subscriber_) { iox2_subscriber_drop(subscriber_); subscriber_ = nullptr; }
		if (service_) { iox2_port_factory_pub_sub_drop(service_); service_ = nullptr; }
		if (name_) { iox2_service_name_drop(name_); name_ = nullptr; }
		if (node_) { iox2_node_drop(node_); node_ = nullptr; }
	}

	~ProgressSubscriber() { close(); }

private:
	iox2_node_h node_ = nullptr;
	iox2_service_name_h name_ = nullptr;
	iox2_port_factory_pub_sub_h service_ = nullptr;
	iox2_subscriber_h subscriber_ = nullptr;
};

} // namespace cineform

#endif
