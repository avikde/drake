#include "drake/multibody/multibody_tree/multibody_plant/multibody_plant.h"

#include <functional>
#include <limits>
#include <memory>

#include <gtest/gtest.h>

#include "drake/common/test_utilities/eigen_matrix_compare.h"
#include "drake/geometry/geometry_system.h"
#include "drake/multibody/benchmarks/acrobot/acrobot.h"
#include "drake/multibody/benchmarks/acrobot/make_acrobot_plant.h"
#include "drake/multibody/benchmarks/pendulum/make_pendulum_plant.h"
#include "drake/multibody/multibody_tree/joints/revolute_joint.h"
#include "drake/multibody/multibody_tree/rigid_body.h"
#include "drake/multibody/multibody_tree/test_utilities/expect_error_message.h"
#include "drake/systems/framework/context.h"
#include "drake/systems/framework/continuous_state.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/systems/primitives/linear_system.h"

namespace drake {

using Eigen::AngleAxisd;
using Eigen::Isometry3d;
using Eigen::Matrix2d;
using Eigen::Vector2d;
using Eigen::Vector3d;
using Eigen::VectorXd;
using geometry::FrameId;
using geometry::FrameIdVector;
using geometry::FramePoseVector;
using geometry::GeometrySystem;
using multibody::benchmarks::Acrobot;
using multibody::benchmarks::acrobot::AcrobotParameters;
using multibody::benchmarks::acrobot::MakeAcrobotPlant;
using multibody::benchmarks::pendulum::MakePendulumPlant;
using multibody::benchmarks::pendulum::PendulumParameters;
using systems::AbstractValue;
using systems::BasicVector;
using systems::Context;
using systems::ContinuousState;
using systems::DiagramBuilder;
using systems::Diagram;
using systems::LinearSystem;
using systems::Linearize;
using systems::VectorBase;

namespace multibody {
namespace multibody_plant {
namespace {

// This test creates a simple model for an acrobot using MultibodyPlant and
// verifies a number of invariants such as that body and joint models were
// properly added and the model sizes.
GTEST_TEST(MultibodyPlant, SimpleModelCreation) {
  const std::string kInvalidName = "InvalidName";

  const AcrobotParameters parameters;
  std::unique_ptr<MultibodyPlant<double>> plant =
      MakeAcrobotPlant(parameters, true /* Make a finalized plant. */);

  // MakeAcrobotPlant() has already called Finalize() on the new acrobot plant.
  // Therefore attempting to call this method again will throw an exception.
  // Verify this.
  EXPECT_THROW(plant->Finalize(), std::logic_error);

  // Model Size. Counting the world body, there should be three bodies.
  EXPECT_EQ(plant->num_bodies(), 3);
  EXPECT_EQ(plant->num_joints(), 2);
  EXPECT_EQ(plant->num_actuators(), 1);
  EXPECT_EQ(plant->num_actuated_dofs(), 1);

  // State size.
  EXPECT_EQ(plant->num_positions(), 2);
  EXPECT_EQ(plant->num_velocities(), 2);
  EXPECT_EQ(plant->num_multibody_states(), 4);

  // Query if elements exist in the model.
  EXPECT_TRUE(plant->HasBodyNamed(parameters.link1_name()));
  EXPECT_TRUE(plant->HasBodyNamed(parameters.link2_name()));
  EXPECT_FALSE(plant->HasBodyNamed(kInvalidName));

  EXPECT_TRUE(plant->HasJointNamed(parameters.shoulder_joint_name()));
  EXPECT_TRUE(plant->HasJointNamed(parameters.elbow_joint_name()));
  EXPECT_FALSE(plant->HasJointNamed(kInvalidName));

  EXPECT_TRUE(plant->HasJointActuatorNamed(parameters.actuator_name()));
  EXPECT_FALSE(plant->HasJointActuatorNamed(kInvalidName));

  // Get links by name.
  const Body<double>& link1 = plant->GetBodyByName(parameters.link1_name());
  EXPECT_EQ(link1.name(), parameters.link1_name());
  const Body<double>& link2 = plant->GetBodyByName(parameters.link2_name());
  EXPECT_EQ(link2.name(), parameters.link2_name());

  // Attempting to retrieve a link that is not part of the model should throw
  // an exception.
  EXPECT_THROW(plant->GetBodyByName(kInvalidName), std::logic_error);

  // Get joints by name.
  const Joint<double>& shoulder_joint =
      plant->GetJointByName(parameters.shoulder_joint_name());
  EXPECT_EQ(shoulder_joint.name(), parameters.shoulder_joint_name());
  const Joint<double>& elbow_joint =
      plant->GetJointByName(parameters.elbow_joint_name());
  EXPECT_EQ(elbow_joint.name(), parameters.elbow_joint_name());
  EXPECT_THROW(plant->GetJointByName(kInvalidName), std::logic_error);

  // Templatized version to obtain retrieve a particular known type of joint.
  const RevoluteJoint<double>& shoulder =
      plant->GetJointByName<RevoluteJoint>(parameters.shoulder_joint_name());
  EXPECT_EQ(shoulder.name(), parameters.shoulder_joint_name());
  const RevoluteJoint<double>& elbow =
      plant->GetJointByName<RevoluteJoint>(parameters.elbow_joint_name());
  EXPECT_EQ(elbow.name(), parameters.elbow_joint_name());
  EXPECT_THROW(plant->GetJointByName(kInvalidName), std::logic_error);

  // MakeAcrobotPlant() has already called Finalize() on the acrobot model.
  // Therefore no more modeling elements can be added. Verify this.
  DRAKE_EXPECT_ERROR_MESSAGE(
      plant->AddRigidBody("AnotherBody", SpatialInertia<double>()),
      std::logic_error,
      /* Verify this method is throwing for the right reasons. */
      "Post-finalize calls to '.*' are not allowed; "
      "calls to this method must happen before Finalize\\(\\).");
  DRAKE_EXPECT_ERROR_MESSAGE(
      plant->AddJoint<RevoluteJoint>(
          "AnotherJoint", link1, {}, link2, {}, Vector3d::UnitZ()),
      std::logic_error,
      /* Verify this method is throwing for the right reasons. */
      "Post-finalize calls to '.*' are not allowed; "
      "calls to this method must happen before Finalize\\(\\).");
  // TODO(amcastro-tri): add test to verify that requesting a joint of the wrong
  // type throws an exception. We need another joint type to do so.
}

// Fixture to perform a number of computational tests on an acrobot model.
class AcrobotPlantTests : public ::testing::Test {
 public:
  // Creates MultibodyPlant for an acrobot model.
  void SetUp() override {
    systems::DiagramBuilder<double> builder;
    geometry_system_ = builder.AddSystem<GeometrySystem>();
    // Make a non-finalized plant so that we can tests methods with pre/post
    // Finalize() conditions.
    plant_ = builder.AddSystem(
        MakeAcrobotPlant(parameters_, false, geometry_system_));
    // Sanity check on the availability of the optional source id before using
    // it.
    DRAKE_DEMAND(plant_->get_source_id() != nullopt);

    // Verify that methods with pre-Finalize() conditions throw accordingly.
    DRAKE_EXPECT_ERROR_MESSAGE(
        plant_->get_geometry_ids_output_port(),
        std::logic_error,
        /* Verify this method is throwing for the right reasons. */
        "Pre-finalize calls to '.*' are not allowed; "
        "you must call Finalize\\(\\) first.");

    DRAKE_EXPECT_ERROR_MESSAGE(
        plant_->get_geometry_poses_output_port(),
        std::logic_error,
        /* Verify this method is throwing for the right reasons. */
        "Pre-finalize calls to '.*' are not allowed; "
        "you must call Finalize\\(\\) first.");

    DRAKE_EXPECT_ERROR_MESSAGE(
        plant_->get_continuous_state_output_port(),
        std::logic_error,
        /* Verify this method is throwing for the right reasons. */
        "Pre-finalize calls to '.*' are not allowed; "
        "you must call Finalize\\(\\) first.");

    // Finalize() the plant before accessing its ports for communicating with
    // GeometrySystem.
    plant_->Finalize();

    builder.Connect(
        plant_->get_geometry_ids_output_port(),
        geometry_system_->get_source_frame_id_port(
            plant_->get_source_id().value()));
    builder.Connect(
        plant_->get_geometry_poses_output_port(),
        geometry_system_->get_source_pose_port(
            plant_->get_source_id().value()));
    // And build the Diagram:
    diagram_ = builder.Build();

    link1_ = &plant_->GetBodyByName(parameters_.link1_name());
    link2_ = &plant_->GetBodyByName(parameters_.link2_name());
    shoulder_ = &plant_->GetJointByName<RevoluteJoint>(
        parameters_.shoulder_joint_name());
    elbow_ = &plant_->GetJointByName<RevoluteJoint>(
        parameters_.elbow_joint_name());

    context_ = plant_->CreateDefaultContext();
    derivatives_ = plant_->AllocateTimeDerivatives();

    ASSERT_GT(plant_->num_actuators(), 0);
    input_port_ = &context_->FixInputPort(
        plant_->get_actuation_input_port().get_index(), Vector1<double>(0.0));
  }

  // Verifies the computation performed by MultibodyPlant::CalcTimeDerivatives()
  // for the acrobot model. The comparison is carried out against a benchmark
  // with hand written dynamics.
  void VerifyCalcTimeDerivatives(double theta1, double theta2,
                                 double theta1dot, double theta2dot,
                                 double input_torque) {
    const double kTolerance = 5 * std::numeric_limits<double>::epsilon();

    // Set the state:
    shoulder_->set_angle(context_.get(), theta1);
    elbow_->set_angle(context_.get(), theta2);
    shoulder_->set_angular_rate(context_.get(), theta1dot);
    elbow_->set_angular_rate(context_.get(), theta2dot);

    // Fix input port to a value before computing anything. In this case, zero
    // actuation.
    input_port_->GetMutableVectorData<double>()->SetAtIndex(0, input_torque);

    plant_->CalcTimeDerivatives(*context_, derivatives_.get());
    const VectorXd xdot = derivatives_->CopyToVector();

    // Now compute inverse dynamics using our benchmark:
    Vector2d C_expected = acrobot_benchmark_.CalcCoriolisVector(
        theta1, theta2, theta1dot, theta2dot);
    Vector2d tau_g_expected =
        acrobot_benchmark_.CalcGravityVector(theta1, theta2);
    Vector2d rhs = tau_g_expected - C_expected + Vector2d(0.0, input_torque);
    Matrix2d M_expected = acrobot_benchmark_.CalcMassMatrix(theta2);
    Vector2d vdot_expected = M_expected.inverse() * rhs;
    VectorXd xdot_expected(4);
    xdot_expected << Vector2d(theta1dot, theta2dot), vdot_expected;

    EXPECT_TRUE(CompareMatrices(
        xdot, xdot_expected, kTolerance, MatrixCompareType::relative));
  }

 protected:
  // The parameters of the model:
  const AcrobotParameters parameters_;
  // The model plant:
  MultibodyPlant<double>* plant_{nullptr};
  // A GeometrySystem so that we can test geometry registration.
  GeometrySystem<double>* geometry_system_{nullptr};
  // The Diagram containing both the MultibodyPlant and the GeometrySystem.
  std::unique_ptr<Diagram<double>> diagram_;
  // Workspace including context and derivatives vector:
  std::unique_ptr<Context<double>> context_;
  std::unique_ptr<ContinuousState<double>> derivatives_;
  // Non-owning pointers to the model's elements:
  const Body<double>* link1_{nullptr};
  const Body<double>* link2_{nullptr};
  const RevoluteJoint<double>* shoulder_{nullptr};
  const RevoluteJoint<double>* elbow_{nullptr};
  // Input port for the actuation:
  systems::FreestandingInputPortValue* input_port_{nullptr};

  // Reference benchmark for verification.
  Acrobot<double> acrobot_benchmark_{
      Vector3d::UnitZ() /* Plane normal */, Vector3d::UnitY() /* Up vector */,
      parameters_.m1(), parameters_.m2(),
      parameters_.l1(), parameters_.l2(),
      parameters_.lc1(), parameters_.lc2(),
      parameters_.Ic1(), parameters_.Ic2(),
      parameters_.b1(), parameters_.b2(),
      parameters_.g()};
};

// Verifies the correctness of MultibodyPlant::CalcTimeDerivatives() on a model
// of an acrobot.
TEST_F(AcrobotPlantTests, CalcTimeDerivatives) {
  // Some random tests with non-zero state:
  VerifyCalcTimeDerivatives(
      -M_PI / 5.0, M_PI / 2.0,  /* joint's angles */
      0.5, 1.0,                 /* joint's angular rates */
      -1.0);                    /* Actuation torque */
  VerifyCalcTimeDerivatives(
      M_PI / 3.0, -M_PI / 5.0,  /* joint's angles */
      0.7, -1.0,                /* joint's angular rates */
      1.0);                     /* Actuation torque */
  VerifyCalcTimeDerivatives(
      M_PI / 4.0, -M_PI / 3.0,  /* joint's angles */
      -0.5, 2.0,                /* joint's angular rates */
      -1.5);                    /* Actuation torque */
  VerifyCalcTimeDerivatives(
      -M_PI, -M_PI / 2.0,       /* joint's angles */
      -1.5, -2.5,               /* joint's angular rates */
      2.0);                     /* Actuation torque */
}

// Verifies the process of geometry registration with a GeometrySystem for the
// acrobot model.
TEST_F(AcrobotPlantTests, GeometryRegistration) {
  EXPECT_EQ(plant_->get_num_visual_geometries(), 3);
  EXPECT_TRUE(plant_->geometry_source_is_registered());
  EXPECT_TRUE(plant_->get_source_id());

  // The default context gets initialized by a call to SetDefaultState(), which
  // for a MultibodyPlant sets all revolute joints to have zero angles and zero
  // angular velocity.
  std::unique_ptr<systems::Context<double>> context =
      plant_->CreateDefaultContext();

  std::unique_ptr<AbstractValue> ids_value =
      plant_->get_geometry_ids_output_port().Allocate(*context);
  EXPECT_NO_THROW(ids_value->GetValueOrThrow<FrameIdVector>());
  const FrameIdVector& ids = ids_value->GetValueOrThrow<FrameIdVector>();
  EXPECT_EQ(ids.get_source_id(), plant_->get_source_id());
  EXPECT_EQ(ids.size(), 2);  // Only two frames move.

  std::unique_ptr<AbstractValue> poses_value =
      plant_->get_geometry_poses_output_port().Allocate(*context);
  EXPECT_NO_THROW(poses_value->GetValueOrThrow<FramePoseVector<double>>());
  const FramePoseVector<double>& poses =
      poses_value->GetValueOrThrow<FramePoseVector<double>>();
  EXPECT_EQ(poses.get_source_id(), plant_->get_source_id());
  EXPECT_EQ(poses.vector().size(), 2);  // Only two frames move.

  // Compute the poses for each geometry in the model.
  plant_->get_geometry_poses_output_port().Calc(*context, poses_value.get());

  const MultibodyTree<double>& model = plant_->model();
  std::vector<Isometry3<double >> X_WB_all;
  model.CalcAllBodyPosesInWorld(*context, &X_WB_all);
  const double kTolerance = 5 * std::numeric_limits<double>::epsilon();
  for (BodyIndex body_index(1);
       body_index < plant_->num_bodies(); ++body_index) {
    const FrameId frame_id = plant_->GetBodyFrameIdOrThrow(body_index);
    const int id_index = ids.GetIndex(frame_id);
    const Isometry3<double>& X_WB = poses.vector()[id_index];
    const Isometry3<double>& X_WB_expected = X_WB_all[body_index];
    EXPECT_TRUE(CompareMatrices(X_WB.matrix(), X_WB_expected.matrix(),
                                kTolerance, MatrixCompareType::relative));
  }

  // GeometrySystem does not register a FrameId for the world. We use this fact
  // to test that GetBodyFrameIdOrThrow() throws an assertion for a body with no
  // FrameId, even though in this model we register an anchored geometry to the
  // world.
  DRAKE_EXPECT_ERROR_MESSAGE(
      plant_->GetBodyFrameIdOrThrow(world_index()),
      std::logic_error,
      /* Verify this method is throwing for the right reasons. */
      "Body 'WorldBody' does not have geometry registered with it.");
}

GTEST_TEST(MultibodyPlantTest, LinearizePendulum) {
  const double kTolerance = 5 * std::numeric_limits<double>::epsilon();

  PendulumParameters parameters;
  std::unique_ptr<MultibodyPlant<double>> pendulum =
      MakePendulumPlant(parameters);
  const auto& pin =
      pendulum->GetJointByName<RevoluteJoint>(parameters.pin_joint_name());
  std::unique_ptr<Context<double>> context = pendulum->CreateDefaultContext();
  context->FixInputPort(0, Vector1d{0.0});

  // First we will linearize about the unstable fixed point with the pendulum
  // in its inverted position.
  pin.set_angle(context.get(), M_PI);
  pin.set_angular_rate(context.get(), 0.0);

  std::unique_ptr<LinearSystem<double>> linearized_pendulum =
      Linearize(*pendulum, *context,
                pendulum->get_actuation_input_port().get_index(),
                systems::kNoOutput);

  // Compute the expected solution by hand.
  Eigen::Matrix2d A;
  Eigen::Vector2d B;
  A <<                            0.0, 1.0,
      parameters.g() / parameters.l(), 0.0;
  B << 0, 1 / (parameters.m()* parameters.l() * parameters.l());
  EXPECT_TRUE(CompareMatrices(linearized_pendulum->A(), A, kTolerance));
  EXPECT_TRUE(CompareMatrices(linearized_pendulum->B(), B, kTolerance));

  // Now we linearize about the stable fixed point with the pendulum in its
  // downward position.
  pin.set_angle(context.get(), 0.0);
  pin.set_angular_rate(context.get(), 0.0);
  linearized_pendulum = Linearize(
      *pendulum, *context,
      pendulum->get_actuation_input_port().get_index(), systems::kNoOutput);
  // Compute the expected solution by hand.
  A <<                             0.0, 1.0,
      -parameters.g() / parameters.l(), 0.0;
  B << 0, 1 / (parameters.m()* parameters.l() * parameters.l());
  EXPECT_TRUE(CompareMatrices(linearized_pendulum->A(), A, kTolerance));
  EXPECT_TRUE(CompareMatrices(linearized_pendulum->B(), B, kTolerance));
}

TEST_F(AcrobotPlantTests, EvalContinuousStateOutputPort) {
  EXPECT_EQ(plant_->get_num_visual_geometries(), 3);
  EXPECT_TRUE(plant_->geometry_source_is_registered());
  EXPECT_TRUE(plant_->get_source_id());

  // The default context gets initialized by a call to SetDefaultState(), which
  // for a MultibodyPlant sets all revolute joints to have zero angles and zero
  // angular velocity.
  std::unique_ptr<systems::Context<double>> context =
      plant_->CreateDefaultContext();

  // Set some non-zero state:
  shoulder_->set_angle(context.get(), M_PI / 3.0);
  elbow_->set_angle(context.get(), -0.2);
  shoulder_->set_angular_rate(context.get(), -0.5);
  elbow_->set_angular_rate(context.get(), 2.5);

  std::unique_ptr<AbstractValue> state_value =
      plant_->get_continuous_state_output_port().Allocate(*context);
  EXPECT_NO_THROW(state_value->GetValueOrThrow<BasicVector<double>>());
  const BasicVector<double>& state_out =
      state_value->GetValueOrThrow<BasicVector<double>>();
  EXPECT_EQ(state_out.size(), plant_->num_multibody_states());

  // Compute the poses for each geometry in the model.
  plant_->get_continuous_state_output_port().Calc(*context, state_value.get());

  // Get continuous state_out from context.
  const VectorBase<double>& state = context->get_continuous_state_vector();

  // Verify state_out indeed matches state.
  EXPECT_EQ(state_out.CopyToVector(), state.CopyToVector());
}

GTEST_TEST(MultibodyPlantTest, MapVelocityToQdotAndBack) {
  MultibodyPlant<double> plant;
  // This test is purely kinematic. Therefore we leave the spatial inertia
  // initialized to garbage. It should not affect the results.
  const RigidBody<double>& body =
      plant.AddRigidBody("FreeBody", SpatialInertia<double>());
  plant.Finalize();
  std::unique_ptr<Context<double>> context = plant.CreateDefaultContext();

  // Set an arbitrary pose of the body in the world.
  const Vector3d p_WB(1, 2, 3);  // Position in world.
  const Vector3d axis_W =        // Orientation in world.
      (1.5 * Vector3d::UnitX() +
       2.0 * Vector3d::UnitY() +
       3.0 * Vector3d::UnitZ()).normalized();
  Isometry3d X_WB = Isometry3d::Identity();
  X_WB.linear() = AngleAxisd(M_PI / 3.0, axis_W).toRotationMatrix();
  X_WB.translation() = p_WB;
  plant.model().SetFreeBodyPoseOrThrow(body, X_WB, context.get());

  // Set an arbitrary, non-zero, spatial velocity of B in W.
  const SpatialVelocity<double> V_WB(Vector3d(1.0, 2.0, 3.0),
                                     Vector3d(-1.0, 4.0, -0.5));
  plant.model().SetFreeBodySpatialVelocityOrThrow(body, V_WB, context.get());

  // Use of MultibodyPlant's mapping to convert generalized velocities to time
  // derivatives of generalized coordinates.
  BasicVector<double> qdot(plant.num_positions());
  BasicVector<double> v(plant.num_velocities());
  ASSERT_EQ(qdot.size(), 7);
  ASSERT_EQ(v.size(), 6);
  v.SetFrom(context->get_continuous_state().get_generalized_velocity());
  plant.MapVelocityToQDot(*context, v, &qdot);

  // Mapping from qdot back to v should result in the original vector of
  // generalized velocities. Verify this.
  BasicVector<double> v_back(plant.num_velocities());
  plant.MapQDotToVelocity(*context, qdot, &v_back);

  const double kTolerance = 5 * std::numeric_limits<double>::epsilon();
  EXPECT_TRUE(
      CompareMatrices(v_back.CopyToVector(), v.CopyToVector(), kTolerance));
}

}  // namespace
}  // namespace multibody_plant
}  // namespace multibody
}  // namespace drake
