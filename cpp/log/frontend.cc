/* -*- indent-tabs-mode: nil -*- */
#include "log/frontend.h"

#include <glog/logging.h>

#include "log/cert.h"
#include "log/cert_submission_handler.h"
#include "log/frontend_signer.h"
#include "monitoring/event_metric.h"
#include "proto/ct.pb.h"
#include "util/status.h"

using cert_trans::CertChain;
using cert_trans::PreCertChain;
using ct::LogEntry;
using ct::SignedCertificateTimestamp;
using std::string;
using std::lock_guard;
using std::mutex;
using util::Status;

namespace {

static cert_trans::EventMetric<std::string, std::string>
    submission_status_metric(
        "submission_status", "entry_type", "status",
        "Submission status totals broken down by entry type and status code.");

Status GetSubmitError(CertSubmissionHandler::SubmitResult result) {
  CHECK_NE(result, CertSubmissionHandler::OK);

  switch (result) {
    case CertSubmissionHandler::EMPTY_SUBMISSION:
      return util::Status(util::error::INVALID_ARGUMENT, "empty submission");
    case CertSubmissionHandler::INVALID_PEM_ENCODED_CHAIN:
      return util::Status(util::error::INVALID_ARGUMENT,
                          "invalid PEM encoded chain");
    case CertSubmissionHandler::SUBMISSION_TOO_LONG:
      return util::Status(util::error::INVALID_ARGUMENT,
                          "submission too long");
    case CertSubmissionHandler::INVALID_CERTIFICATE_CHAIN:
      return util::Status(util::error::INVALID_ARGUMENT,
                          "invalid certificate chain");
    case CertSubmissionHandler::UNKNOWN_ROOT:
      return util::Status(util::error::FAILED_PRECONDITION, "unknown root");
    case CertSubmissionHandler::PRECERT_CHAIN_NOT_WELL_FORMED:
      return util::Status(util::error::INVALID_ARGUMENT,
                          "prechain not well formed");
    case CertSubmissionHandler::INTERNAL_ERROR:
      return util::Status(util::error::INTERNAL, "internal error");
    default:
      LOG(FATAL) << "Unknown CertSubmissionHandler return code " << result;
  }
}

Status UpdateStats(ct::LogEntryType type, const Status& status) {
  if (type == ct::X509_ENTRY) {
    submission_status_metric.RecordEvent(
        "x509", util::ErrorCodeString(status.CanonicalCode()), 1);
  } else {
    submission_status_metric.RecordEvent(
        "precert", util::ErrorCodeString(status.CanonicalCode()), 1);
  }
  return status;
}

}  // namespace

Frontend::Frontend(CertSubmissionHandler* handler, FrontendSigner* signer)
    : handler_(handler), signer_(signer) {
}

Frontend::~Frontend() {
  delete signer_;
  delete handler_;
}

Status Frontend::QueueProcessedEntry(
    CertSubmissionHandler::SubmitResult pre_result, const LogEntry& entry,
    SignedCertificateTimestamp* sct) {
  if (pre_result != CertSubmissionHandler::OK) {
    return UpdateStats(entry.type(), GetSubmitError(pre_result));
  }

  // Step 2. Submit to database.
  return UpdateStats(entry.type(), signer_->QueueEntry(entry, sct));
}

Status Frontend::QueueX509Entry(CertChain* chain,
                                SignedCertificateTimestamp* sct) {
  LogEntry entry;
  // Make sure the correct statistics get updated in case of error.
  entry.set_type(ct::X509_ENTRY);
  return QueueProcessedEntry(handler_->ProcessX509Submission(chain, &entry),
                             entry, sct);
}

Status Frontend::QueuePreCertEntry(PreCertChain* chain,
                                   SignedCertificateTimestamp* sct) {
  LogEntry entry;
  // Make sure the correct statistics get updated in case of error.
  entry.set_type(ct::PRECERT_ENTRY);
  return QueueProcessedEntry(handler_->ProcessPreCertSubmission(chain, &entry),
                             entry, sct);
}
